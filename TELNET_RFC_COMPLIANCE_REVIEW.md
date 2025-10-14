# Telnet RFC 준수 상태 검토 보고서

## 검토 일자
2025년 10월 15일

## 검토 기준
- RFC 854: Telnet Protocol Specification
- RFC 855: Telnet Option Negotiation
- RFC 858: Suppress Go Ahead (SGA)
- RFC 1184: Telnet Linemode Option
- RFC 1091: Telnet Terminal-Type Option

## 요약

현재 ModemBridge의 telnet 구현은 **기본 RFC 854/855/858은 대부분 준수**하지만, **RFC 1184 (LINEMODE)와 RFC 1091 (TERMINAL-TYPE)은 미구현** 상태입니다. 또한 일부 IAC 명령과 방어적 처리가 부족합니다.

---

## 1. RFC 854: 기본 IAC/명령 처리

### 요구사항
- IAC(255)로 시작하는 명령 파싱
- SE(240), SB(250), WILL/WONT/DO/DONT(251-254), GA(249) 등 처리
- 데이터 내 0xFF는 IAC IAC로 이스케이프
- Synch/DM, IP/AO/AYT/EC/EL/BRK 처리

### 현재 구현 상태

#### ✓ 구현된 기능 (src/telnet.c)

**1. IAC 명령 파싱** (line 315-410)
```c
case TELNET_STATE_IAC:
    if (c == TELNET_IAC) {
        /* Escaped IAC - output single IAC */
        output[out_pos++] = TELNET_IAC;  // ✓ IAC IAC → 0xFF 복원
    } else if (c == TELNET_WILL) {
        tn->state = TELNET_STATE_WILL;   // ✓
    } else if (c == TELNET_WONT) {
        tn->state = TELNET_STATE_WONT;   // ✓
    } else if (c == TELNET_DO) {
        tn->state = TELNET_STATE_DO;     // ✓
    } else if (c == TELNET_DONT) {
        tn->state = TELNET_STATE_DONT;   // ✓
    } else if (c == TELNET_SB) {
        tn->state = TELNET_STATE_SB;     // ✓
    }
```

**2. IAC 이스케이프 (송신)** (line 437-455)
```c
if (c == TELNET_IAC) {
    /* Escape IAC by doubling it */
    output[out_pos++] = TELNET_IAC;
    output[out_pos++] = TELNET_IAC;  // ✓ 0xFF → IAC IAC
}
```

**3. 서브협상 프레이밍** (line 372-402)
```c
case TELNET_STATE_SB:
    if (c == TELNET_IAC) {
        tn->state = TELNET_STATE_SB_IAC;
    }
    break;

case TELNET_STATE_SB_IAC:
    if (c == TELNET_SE) {
        /* End of subnegotiation */
        telnet_handle_subnegotiation(tn);  // ✓
    } else if (c == TELNET_IAC) {
        /* Escaped IAC in subnegotiation */
        tn->sb_buffer[tn->sb_len++] = TELNET_IAC;  // ✓
    }
    break;
```

#### ✗ 미구현 또는 불완전

**1. IAC 명령 처리 불완전** (line 346-349)
```c
else {
    /* Other IAC commands - just log */
    MB_LOG_DEBUG("Received IAC command: %d", c);
    tn->state = TELNET_STATE_DATA;  // ✗ 로그만 하고 무시
}
```

**문제**: 다음 명령들이 무시됨
- TELNET_GA (249) - Go Ahead
- TELNET_EL (248) - Erase Line
- TELNET_EC (247) - Erase Character
- TELNET_AYT (246) - Are You There
- TELNET_AO (245) - Abort Output
- TELNET_IP (244) - Interrupt Process
- TELNET_BREAK (243) - Break
- TELNET_DM (242) - Data Mark
- TELNET_NOP (241) - No Operation
- TELNET_EOR (239) - End of Record

**권장**: 최소한 NOP와 GA는 처리 (GA는 SGA 협상 시 무시 가능)

**2. TCP Urgent/Synch 미지원**
- DM(Data Mark)과 TCP Urgent 포인터 조합 미처리
- 긴급 데이터 처리 없음

---

## 2. RFC 855: 옵션 협상 원칙

### 요구사항
- DO/DONT(상대에게 수행 요청/중지), WILL/WONT(자신이 수행/거부)
- 각 방향 독립적 협상
- 미지원 옵션에는 즉시 WONT/DONT 응답

### 현재 구현 상태

#### ✓ 구현된 기능

**1. 기본 협상 구조** (line 228-314)
```c
switch (command) {
    case TELNET_WILL:
        if (option == TELOPT_BINARY || option == TELOPT_SGA || option == TELOPT_ECHO) {
            tn->remote_options[option] = true;
            telnet_send_negotiate(tn, TELNET_DO, option);  // ✓ 지원 옵션 수락
        } else {
            telnet_send_negotiate(tn, TELNET_DONT, option);  // ✓ 미지원 거부
        }
        break;

    case TELNET_WONT:
        tn->remote_options[option] = false;
        telnet_send_negotiate(tn, TELNET_DONT, option);  // ✓
        break;

    case TELNET_DO:
        if (option == TELOPT_BINARY || option == TELOPT_SGA) {
            tn->local_options[option] = true;
            telnet_send_negotiate(tn, TELNET_WILL, option);  // ✓
        } else {
            telnet_send_negotiate(tn, TELNET_WONT, option);  // ✓ 미지원 거부
        }
        break;
    // ...
}
```

**2. 옵션 상태 추적**
```c
bool local_options[256];   // ✓ 로컬 옵션 상태
bool remote_options[256];  // ✓ 원격 옵션 상태
```

#### ✗ 미구현 또는 문제

**1. 협상 루프 방지 미흡**

RFC 855 요구사항:
> "Only acknowledge a change of state (WILL/WONT/DO/DONT)"
> 상태가 변경될 때만 응답, 이미 설정된 상태 재요청은 무시

**현재 문제**:
```c
case TELNET_WILL:
    if (option == TELOPT_BINARY) {
        tn->remote_options[option] = true;
        telnet_send_negotiate(tn, TELNET_DO, option);  // ✗ 항상 응답
    }
```

**개선 필요**:
```c
case TELNET_WILL:
    if (option == TELOPT_BINARY) {
        if (!tn->remote_options[option]) {  // 상태 변화 시만 응답
            tn->remote_options[option] = true;
            telnet_send_negotiate(tn, TELNET_DO, option);
        }
    }
```

**2. 로컬 ECHO 옵션 미지원** (line 271-291)
```c
case TELNET_DO:
    if (option == TELOPT_BINARY || option == TELOPT_SGA) {
        // ✓ BINARY, SGA 지원
    } else {
        telnet_send_negotiate(tn, TELNET_WONT, option);
        // ✗ ECHO 옵션을 로컬에서 거부 (서버가 DO ECHO 보내면 거부)
    }
```

**문제**: 서버가 "DO ECHO" 요청 시 클라이언트가 에코를 수행해야 하는 경우 대응 불가

---

## 3. RFC 858: Suppress Go Ahead (SGA)

### 요구사항
- WILL/DO SGA 협상으로 GA 신호 억제
- SGA와 ECHO를 함께 사용하여 character-at-a-time 모드 구성

### 현재 구현 상태

#### ✓ 구현된 기능

**1. SGA 옵션 협상** (line 201-212, 239-250, 271-284)
```c
// Server WILL SGA 수락
if (option == TELOPT_SGA) {
    tn->remote_options[option] = true;
    telnet_send_negotiate(tn, TELNET_DO, option);  // ✓
    tn->sga_mode = true;
}

// Server DO SGA 수락
if (option == TELOPT_SGA) {
    tn->local_options[option] = true;
    telnet_send_negotiate(tn, TELNET_WILL, option);  // ✓
    tn->sga_mode = true;
}
```

**2. 초기 SGA 협상** (line 102-105)
```c
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_SGA);
telnet_send_negotiate(tn, TELNET_DO, TELOPT_SGA);  // ✓
```

**3. SGA + ECHO 기반 모드 감지** (line 200-214)
```c
if (tn->remote_options[TELOPT_ECHO] && tn->remote_options[TELOPT_SGA]) {
    tn->linemode = false;  // ✓ Character mode
}
```

#### ✗ 문제점

**1. GA 명령 처리 부재**

RFC 858:
> "When in effect, the Go Ahead command must not be sent."

**현재**: GA(249) 명령 자체를 무시 (line 346-349)
```c
else {
    MB_LOG_DEBUG("Received IAC command: %d", c);  // ✗ GA를 그냥 무시
}
```

**권장**:
```c
else if (c == TELNET_GA) {
    /* Go Ahead - ignored when SGA is active */
    if (!tn->sga_mode) {
        MB_LOG_DEBUG("Received GA (not in SGA mode)");
        // Line mode에서 GA는 라인 완료 신호로 사용 가능
    }
    tn->state = TELNET_STATE_DATA;
}
```

---

## 4. RFC 1184: LINEMODE

### 요구사항
- DO/WILL LINEMODE 협상
- SB LINEMODE MODE (EDIT/TRAPSIG/MODE_ACK 등)
- FORWARDMASK (32바이트 비트마스크)
- SLC (Set Local Characters) - 로컬 편집 키 설정

### 현재 구현 상태

#### ✗ **완전 미구현**

**1. LINEMODE 옵션 정의 누락** (include/telnet.h)
```c
#define TELOPT_ECHO         1       /* Echo */
#define TELOPT_SGA          3       /* Suppress go ahead */
// ✗ #define TELOPT_LINEMODE     34      /* Linemode */ 누락
```

**2. LINEMODE 협상 미지원** (src/telnet.c)
- Server DO LINEMODE → 현재는 DONT로 거부
- SB LINEMODE 서브협상 파싱 없음
- MODE/FORWARDMASK/SLC 처리 없음

**3. 현재 동작**:
```c
case TELNET_DO:
    if (option == TELOPT_BINARY || option == TELOPT_SGA) {
        // 지원
    } else {
        telnet_send_negotiate(tn, TELNET_WONT, option);
        // ✗ LINEMODE(34) 거부
    }
```

**영향**:
- 라인 편집 기능 서버에 위임 불가
- 로컬 편집 모드 전환 불가
- 일부 Unix 서버와 호환성 문제 가능

**권장 조치**:
- LINEMODE 옵션 정의 추가
- 기본 LINEMODE 협상 지원 (최소한 MODE 0 지원)
- MODE 서브협상 파싱

---

## 5. RFC 1091: TERMINAL-TYPE

### 요구사항
- DO/WILL TERMINAL-TYPE 협상
- SB TERMINAL-TYPE SEND 요청 처리
- SB TERMINAL-TYPE IS <type> 응답

### 현재 구현 상태

#### ✗ **완전 미구현**

**1. TERMINAL-TYPE 옵션 정의 있음** (include/telnet.h:42)
```c
#define TELOPT_TTYPE        24      /* Terminal type */  // ✓ 정의는 있음
```

**2. 협상 미지원** (src/telnet.c)
```c
case TELNET_DO:
    if (option == TELOPT_BINARY || option == TELOPT_SGA) {
        // 지원
    } else {
        telnet_send_negotiate(tn, TELNET_WONT, option);
        // ✗ TTYPE(24) 거부
    }
```

**3. 서브협상 파싱 미구현**
```c
int telnet_handle_subnegotiation(telnet_t *tn)
{
    // ...
    /* For now, we just log and ignore */
    (void)option;  // ✗ 모든 서브협상 무시
}
```

**영향**:
- 서버가 터미널 타입을 알 수 없음
- VT100, ANSI 등 에뮬레이션 협상 불가
- 일부 BBS/Unix 서버에서 문제 가능

**권장 조치**:
- TERMINAL-TYPE 협상 구현
- 기본 터미널 타입 설정 (예: "ANSI", "VT100", "UNKNOWN")
- SB TERMINAL-TYPE SEND → IS 응답 구현

---

## 6. 방어적 동작과 기본값

### 요구사항
- 기본값은 WONT/DONT
- 협상되지 않은 옵션은 NVT 규칙 (GA 사용 등)
- 각 방향 독립 협상

### 현재 구현 상태

#### ✓ 구현된 기능

**1. 기본값 설정** (line 16-30)
```c
memset(tn->local_options, 0, sizeof(tn->local_options));   // ✓ 기본 false
memset(tn->remote_options, 0, sizeof(tn->remote_options)); // ✓ 기본 false
```

**2. 미지원 옵션 거부** (line 247-249, 287-289)
```c
else {
    telnet_send_negotiate(tn, TELNET_DONT, option);  // ✓ WILL에 DONT
}
// ...
else {
    telnet_send_negotiate(tn, TELNET_WONT, option);  // ✓ DO에 WONT
}
```

#### ✗ 문제점

**1. 각 방향 독립성 미흡**

RFC 855:
> "Enabling an option in one direction does not enable it in the other"

**현재 문제**:
```c
tn->sga_mode = true;  // ✗ 양방향 공통 플래그
```

**개선 필요**:
```c
bool sga_local;   // We suppress GA
bool sga_remote;  // They suppress GA
```

---

## 7. 구현 팁 준수 여부

### 요구사항
- 협상은 상태 변화 시만 재개
- 텔넷 파서는 IAC와 데이터 분리 상태 머신
- SB 페이로드는 IAC SE로만 종료

### 현재 구현 상태

#### ✓ 구현된 기능

**1. 상태 머신 파서** (line 315-410)
```c
switch (tn->state) {
    case TELNET_STATE_DATA:      // ✓
    case TELNET_STATE_IAC:       // ✓
    case TELNET_STATE_WILL:      // ✓
    case TELNET_STATE_WONT:      // ✓
    case TELNET_STATE_DO:        // ✓
    case TELNET_STATE_DONT:      // ✓
    case TELNET_STATE_SB:        // ✓
    case TELNET_STATE_SB_IAC:    // ✓
}
```

**2. SB 종료 감지** (line 384-388)
```c
if (c == TELNET_SE) {
    telnet_handle_subnegotiation(tn);  // ✓ IAC SE로만 종료
}
```

#### ✗ 문제점

**1. 협상 루프 방지 미흡** (앞서 언급)
- 상태 변화 체크 없이 항상 응답

---

## 준수 요약표

| RFC | 항목 | 구현 상태 | 완성도 |
|-----|------|-----------|--------|
| **RFC 854** | IAC 파싱 | ✓ 부분 구현 | 70% |
| | IAC 이스케이프 | ✓ 완전 구현 | 100% |
| | SB/SE 프레이밍 | ✓ 완전 구현 | 100% |
| | GA/AYT/IP/AO 등 | ✗ 미구현 | 0% |
| | TCP Urgent/Synch | ✗ 미구현 | 0% |
| **RFC 855** | 기본 협상 구조 | ✓ 구현 | 90% |
| | 협상 루프 방지 | ✗ 미흡 | 30% |
| | 미지원 옵션 거부 | ✓ 구현 | 100% |
| | 각 방향 독립성 | ✗ 부분 미흡 | 70% |
| **RFC 858** | SGA 협상 | ✓ 완전 구현 | 100% |
| | GA 처리 | ✗ 미구현 | 0% |
| | Character mode 감지 | ✓ 구현 | 100% |
| **RFC 1184** | LINEMODE 협상 | ✗ 완전 미구현 | 0% |
| | MODE 서브협상 | ✗ 미구현 | 0% |
| | FORWARDMASK | ✗ 미구현 | 0% |
| | SLC | ✗ 미구현 | 0% |
| **RFC 1091** | TERMINAL-TYPE 협상 | ✗ 미구현 | 0% |
| | SEND/IS 서브협상 | ✗ 미구현 | 0% |

**전체 완성도: 약 45%**

---

## 우선순위별 권장 수정 사항

### 우선순위 1 (필수)

**1. 협상 루프 방지**
- 옵션 상태 변화 시에만 응답
- 이미 설정된 옵션 재요청 무시

**2. NOP/GA 명령 처리**
- 최소한 무시하는 대신 적절히 처리
- GA는 SGA 모드 여부에 따라 처리

**3. 각 방향 독립적 옵션 상태**
- `sga_mode` → `sga_local`, `sga_remote` 분리
- `binary_mode` 동일하게 분리

### 우선순위 2 (중요)

**4. TERMINAL-TYPE 기본 지원**
```c
#define TELOPT_TTYPE 24
#define TTYPE_IS   0
#define TTYPE_SEND 1

// 협상
case TELNET_DO:
    if (option == TELOPT_TTYPE) {
        tn->local_options[option] = true;
        telnet_send_negotiate(tn, TELNET_WILL, option);
    }

// 서브협상 응답
void handle_ttype_subnegotiation(telnet_t *tn) {
    if (tn->sb_buffer[1] == TTYPE_SEND) {
        // Send: IAC SB TTYPE IS "ANSI" IAC SE
        unsigned char response[] = {
            IAC, SB, TELOPT_TTYPE, TTYPE_IS,
            'A', 'N', 'S', 'I',
            IAC, SE
        };
        send(tn->fd, response, sizeof(response), 0);
    }
}
```

**5. 로컬 ECHO 옵션 지원 (선택적)**
- 서버가 DO ECHO 요청 시 수락 가능하도록
- ModemBridge 특성상 필요성 낮음 (항상 서버 에코 선호)

### 우선순위 3 (향후)

**6. LINEMODE 기본 지원**
- MODE 0 (character mode) 지원
- 라인 편집 기능은 미구현 가능 (EDIT=0)

**7. 나머지 IAC 명령**
- AYT, IP, AO, BREAK 등
- 실제 사용 빈도 낮음

---

## 실제 영향 평가

### 현재 구현으로 동작하는 서버

✓ **대부분의 기본 텔넷 서버**
- SGA + ECHO만 사용하는 서버
- BBS, MUD 등

✓ **Character mode 서버**
- WILL ECHO + WILL SGA만 사용
- 현재 구현으로 정상 동작

### 문제가 발생할 수 있는 서버

✗ **LINEMODE를 요구하는 Unix 서버**
- DO LINEMODE 요청 시 WONT 응답
- 서버가 fallback 모드로 동작 (대부분 문제 없음)

✗ **TERMINAL-TYPE을 요구하는 서버**
- DO TTYPE 요청 시 WONT 응답
- 터미널 타입 협상 실패
- 일부 서버에서 기능 제한 또는 레이아웃 문제

✗ **협상 루프 감지하는 엄격한 서버**
- 중복 협상 응답 시 연결 종료 가능
- 실제로는 드뭄

---

## 테스트 권장 사항

### 기본 테스트
```bash
# RFC 854/855/858 준수 확인
telnet telehack.com 23

# Character mode (ECHO + SGA)
telnet discworld.starturtle.net 4242

# TERMINAL-TYPE 요구 서버
telnet mud.arctic.org 2700
```

### 디버그 로그 확인
- 옵션 협상 순서
- 중복 협상 발생 여부
- 거부된 옵션 목록

---

## 결론

ModemBridge의 텔넷 구현은:

1. **RFC 854/855/858 핵심 기능은 양호** (약 70% 준수)
   - IAC 처리, 옵션 협상, SGA 지원 정상
   - 대부분의 텔넷 서버와 호환

2. **RFC 1184/1091은 미구현** (0%)
   - LINEMODE, TERMINAL-TYPE 미지원
   - 일부 서버에서 제한적 동작 가능

3. **개선 필요 영역**
   - 협상 루프 방지
   - GA/NOP 명령 처리
   - 각 방향 독립적 옵션 상태

**권장 조치**: 우선순위 1-2 항목부터 순차 적용
