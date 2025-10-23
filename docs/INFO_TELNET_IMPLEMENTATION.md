# INFO_TELNET_IMPLEMENTATION - Telnet 프로토콜 구현 통합 문서

## 개요
ModemBridge의 Telnet 프로토콜 구현에 대한 통합 기술 문서입니다.
RFC 준수, 문자/라인 모드, 멀티바이트 문자 처리를 포괄합니다.

## 1. RFC 준수 현황

### 1.1 구현 상태 요약
| RFC | 설명 | 구현율 | 상태 |
|-----|------|--------|------|
| RFC 854 | Telnet Protocol | 70% | 핵심 기능 구현 |
| RFC 855 | Option Negotiation | 60% | 기본 협상 구현 |
| RFC 856 | Binary Transmission | 80% | 구현, 일부 제한 |
| RFC 857 | Echo | 50% | 부분 구현 |
| RFC 858 | Suppress Go Ahead | 40% | 기본만 구현 |
| RFC 1091 | Terminal Type | 0% | 미구현 |
| RFC 1184 | Linemode | 0% | 미구현 |
| **전체** | | **45%** | 기본 동작 가능 |

### 1.2 IAC (Interpret As Command) 처리
```c
// RFC 854 핵심 - IAC 이스케이프
// telnet.c:147-178
case TELNET_STATE_DATA:
    if (byte == TELNET_IAC) {
        telnet->state = TELNET_STATE_IAC;
    } else {
        // 일반 데이터 처리
        output_buffer[output_len++] = byte;
    }
    break;

// IAC 이스케이프: 0xFF → 0xFF 0xFF
// 데이터 내 0xFF는 반드시 두 번 전송
```

**구현 상태**: ✅ 완료
- IAC 파싱 정상 동작
- 0xFF 이스케이프 정상 처리
- 상태 기계 안정적

### 1.3 옵션 협상 (DO/DONT/WILL/WONT)
```c
// RFC 855 - 옵션 협상
// 현재 지원 옵션:
#define TELOPT_BINARY   0  // Binary Transmission
#define TELOPT_ECHO     1  // Echo
#define TELOPT_SGA      3  // Suppress Go Ahead

// 미지원 옵션:
#define TELOPT_LINEMODE 34  // Linemode (RFC 1184)
#define TELOPT_TTYPE    24  // Terminal Type (RFC 1091)
```

**문제점**:
1. 초기 협상에서 ECHO 누락
2. Cooperation loop 방지 미흡
3. LINEMODE 미구현으로 라인 편집 제한

## 2. 문자 모드 vs 라인 모드

### 2.1 모드 정의
| 모드 | 특징 | ECHO | SGA | 전송 단위 |
|------|------|------|-----|-----------|
| 문자 모드 | 즉시 전송 | 서버 | ON | 문자 단위 |
| 라인 모드 | 엔터 시 전송 | 클라이언트 | OFF | 라인 단위 |

### 2.2 현재 구현 상태
```c
// telnet.h - 모드 추적
typedef struct {
    bool linemode;      // true: 라인모드, false: 문자모드
    bool local_echo;    // 로컬 에코 여부
    bool remote_echo;   // 원격 에코 여부
    // ...
} telnet_t;

// 문제: 초기화 시 linemode = true로 가정
// 실제로는 서버 협상에 따라 결정되어야 함
```

### 2.3 모드 감지 개선안
```c
// 개선된 모드 감지 로직
void detect_telnet_mode(telnet_t *telnet) {
    // SGA 옵션으로 판단
    if (telnet->options[TELOPT_SGA].enabled) {
        telnet->linemode = false;  // 문자 모드
    }

    // ECHO 옵션으로 판단
    if (telnet->options[TELOPT_ECHO].remote) {
        telnet->local_echo = false;  // 서버 에코
    }
}
```

### 2.4 모뎀 에코와 충돌 방지
```
문제: ATE1 (모뎀 에코) + Telnet 서버 에코 = 이중 에코

해결책:
1. Telnet 연결 시 모뎀 에코 자동 비활성화
2. 또는 Telnet 서버 에코 거부
3. 현재: 수동으로 ATE0 필요 (개선 필요)
```

## 3. 멀티바이트 문자 처리

### 3.1 UTF-8 처리 전략
```c
// UTF-8 시퀀스 길이 판정
int utf8_sequence_length(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0) return 1;      // ASCII
    if ((first_byte & 0xE0) == 0xC0) return 2;   // 2-byte
    if ((first_byte & 0xF0) == 0xE0) return 3;   // 3-byte
    if ((first_byte & 0xF8) == 0xF0) return 4;   // 4-byte
    return 1;  // 잘못된 시퀀스
}
```

### 3.2 IAC (0xFF) 충돌 문제

#### 인코딩별 0xFF 출현 가능성
| 인코딩 | 0xFF 가능 | 위험도 | 대응 |
|--------|-----------|--------|------|
| UTF-8 | 불가능 | 안전 | 처리 불필요 |
| EUC-KR | 가능 | 높음 | Binary 모드 필수 |
| EUC-JP | 가능 | 높음 | Binary 모드 필수 |
| GB2312 | 가능 | 중간 | Binary 모드 권장 |
| Big5 | 가능 | 높음 | Binary 모드 필수 |

#### Binary 모드 협상
```c
// Binary 모드 요청 (telnet.c)
void request_binary_mode(telnet_t *telnet) {
    // DO BINARY 전송
    uint8_t cmd[] = {TELNET_IAC, TELNET_DO, TELOPT_BINARY};
    send(telnet->sock, cmd, 3, 0);

    // WILL BINARY 전송
    cmd[1] = TELNET_WILL;
    send(telnet->sock, cmd, 3, 0);
}

// 서버가 거부할 경우 경고
if (!telnet->binary_mode) {
    MB_LOG_WARNING("Binary mode rejected - multibyte may fail");
}
```

### 3.3 버퍼 경계 문제
```c
// 문제: 멀티바이트 문자가 버퍼 경계에서 분할
// 해결: 불완전한 시퀀스 보관
typedef struct {
    uint8_t pending[4];     // 불완전한 UTF-8 시퀀스
    int pending_len;        // 보관된 바이트 수
} utf8_buffer_t;

// 버퍼 처리 시
if (is_incomplete_utf8(buffer, len)) {
    save_pending_bytes(utf8_buf, buffer + complete_len);
    process_only_complete(buffer, complete_len);
}
```

### 3.4 권장 버퍼 크기
- **최소**: 1024 bytes (한글 341자)
- **권장**: 4096 bytes (한글 1365자)
- **최대**: 8192 bytes (메모리 고려)

## 4. 구현 문제 및 해결

### 4.1 초기 옵션 협상 누락
**문제**: 연결 직후 ECHO/SGA 협상하지 않음
```c
// 현재 코드 (문제)
int telnet_connect(...) {
    // 연결 후 즉시 반환
    return 0;
}

// 개선 코드
int telnet_connect(...) {
    // 연결 후 초기 협상
    telnet_send_initial_options(telnet);
    return 0;
}
```

### 4.2 GA (Go Ahead) 처리 부재
**문제**: GA 명령 무시로 일부 서버와 호환성 문제
```c
// 개선 필요
case TELNET_GA:
    if (!telnet->suppress_ga) {
        trigger_line_send();  // 라인 전송 트리거
    }
    break;
```

### 4.3 NOP 명령 미처리
**문제**: Keep-alive용 NOP 무시
```c
// 개선 필요
case TELNET_NOP:
    update_last_activity();  // 연결 유지 확인
    break;
```

## 5. 테스트 시나리오

### 5.1 서버 유형별 테스트
| 서버 유형 | 포트 | 모드 | 테스트 항목 |
|-----------|------|------|-------------|
| 문자 모드 MUD | 23 | Character | 실시간 입력, 서버 에코 |
| 라인 모드 BBS | 2323 | Line | 라인 편집, 로컬 에코 |
| 바이너리 전송 | 8023 | Binary | 파일 전송, 0xFF 처리 |
| 한글 BBS | 23232 | Binary+Line | EUC-KR, 멀티바이트 |

### 5.2 프로토콜 준수 테스트
```bash
# Telnet 옵션 협상 모니터링
tcpdump -i lo -X 'port 23'

# IAC 이스케이프 검증
echo -e '\xff\xff' | nc localhost 23

# Binary 모드 테스트
cat binary_file | telnet localhost 23
```

### 5.3 멀티바이트 테스트
```bash
# UTF-8 한글
echo "한글 테스트" | telnet localhost 23

# 버퍼 경계 테스트 (4KB 한글 파일)
cat large_korean.txt | telnet localhost 23

# EUC-KR with 0xFF
echo -e '\xa1\xff' | telnet localhost 23  # Binary 모드 필요
```

## 6. 미구현 기능

### 6.1 RFC 1184 - LINEMODE
- 고급 라인 편집 기능
- SLC (Set Local Characters)
- FORWARDMASK
- 구현 복잡도: 높음

### 6.2 RFC 1091 - TERMINAL-TYPE
- 터미널 타입 협상
- ANSI, VT100 등 지정
- 구현 복잡도: 중간

### 6.3 추가 IAC 명령
- TELNET_EL (Erase Line)
- TELNET_EC (Erase Character)
- TELNET_AYT (Are You There)
- TELNET_AO (Abort Output)
- TELNET_IP (Interrupt Process)
- TELNET_BREAK
- TELNET_DM (Data Mark)
- TELNET_EOR (End of Record)

## 7. 권장 개선 사항

### 우선순위: 높음
1. 초기 옵션 협상 구현
2. 모드 자동 감지 개선
3. 모뎀 에코 자동 조정

### 우선순위: 중간
4. GA/NOP 명령 처리
5. Binary 모드 실패 시 경고 강화
6. 멀티바이트 버퍼 경계 처리

### 우선순위: 낮음
7. LINEMODE 구현
8. TERMINAL-TYPE 구현
9. 추가 IAC 명령 지원

## 8. 참고 자료

### RFC 문서
- [RFC 854](https://www.rfc-editor.org/rfc/rfc854) - Telnet Protocol Specification
- [RFC 855](https://www.rfc-editor.org/rfc/rfc855) - Telnet Option Specifications
- [RFC 856](https://www.rfc-editor.org/rfc/rfc856) - Binary Transmission
- [RFC 857](https://www.rfc-editor.org/rfc/rfc857) - Echo
- [RFC 858](https://www.rfc-editor.org/rfc/rfc858) - Suppress Go Ahead
- [RFC 1091](https://www.rfc-editor.org/rfc/rfc1091) - Terminal Type
- [RFC 1184](https://www.rfc-editor.org/rfc/rfc1184) - Linemode

### 테스트 서버
- `telnet://telnetserver.com` - 다양한 모드 테스트
- `telnet://korea-bbs.com:23232` - 한글 BBS (EUC-KR)
- `telnet://mudserver.com:4000` - MUD 게임 (Character mode)

---
*통합 문서 생성: 2025-10-23*
*원본: INFO_TELNET_MODE_REVIEW.md, INFO_TELNET_RFC_COMPLIANCE.md, INFO_TELNET_MULTIBYTE_REVIEW.md*