# DEV_LEVEL1_TODO - Level 1 남은 작업

## 개요
Level 1 개발에서 아직 구현되지 않았거나 개선이 필요한 작업 목록입니다.

## 우선순위: 높음 🔴

### 1. Line Buffering 완전 통합
**현재 상태**: 부분 구현 (hw_msg_buffer는 있으나 serial_read_line() 미완성)

**필요 작업**:
```c
// serial.c에 추가
typedef struct {
    char buffer[LINE_BUFFER_SIZE];
    size_t pos;
    bool has_partial;
} line_buffer_t;

int serial_read_line(serial_t *serial, char *line,
                    size_t max_len, int timeout_ms);
```

**구현 항목**:
- [ ] serial_t 구조체에 line_buffer 추가
- [ ] serial_read_line() 함수 완성
- [ ] modem.c의 hw_msg_buffer와 통합
- [ ] CONNECT/NO CARRIER 완전한 라인 단위 처리

**예상 효과**: CONNECT 메시지 분할 문제 완전 해결

### 2. UUCP Lock 파일 완전 구현
**현재 상태**: 부분 구현 (lock/unlock 함수는 있으나 UUCP 표준 미준수)

**필요 작업**:
```c
// UUCP 표준 lock 파일: /var/lock/LCK..ttyUSB0
// PID 형식: 10자리 ASCII (예: "     12345\n")

int serial_lock_port_uucp(const char *device) {
    char lockfile[PATH_MAX];
    snprintf(lockfile, sizeof(lockfile), "/var/lock/LCK..%s",
             basename(device));

    // 1. 기존 lock 파일 체크
    // 2. PID 유효성 검증
    // 3. Stale lock 제거
    // 4. 새 lock 생성
}
```

**구현 항목**:
- [ ] UUCP 표준 lock 파일 경로
- [ ] 10자리 PID 형식
- [ ] Stale lock 감지 및 제거
- [ ] 원자적 lock 생성 (O_EXCL)

## 우선순위: 중간 🟡

### 3. 시리얼 초기화 명시적 표시
**현재 상태**: 미구현 (healthcheck_serial_init 함수 없음)

**필요 작업**:
```c
// healthcheck.c에 추가
int healthcheck_serial_init(healthcheck_report_t *report,
                           const serial_t *serial,
                           const config_t *config) {
    printf("\n=== Serial Port Initialization ===\n");
    printf("Port: %s\n", config->serial_port);
    printf("Baudrate: %d\n", config->baudrate_value);
    printf("Settings: %d-%c-%d\n",
           config->data_bits,
           parity_char(config->parity),
           config->stop_bits);
    printf("Flow Control: %s\n",
           flow_control_str(config->flow_control));

    // 실제 초기화
    int result = serial_configure(serial, config);

    printf("Status: %s\n",
           result == SUCCESS ? "OK" : "FAILED");
    return result;
}
```

**구현 항목**:
- [ ] healthcheck.h에 함수 선언 추가
- [ ] healthcheck_run()에 통합
- [ ] 초기화 상태 report에 기록
- [ ] 설정 값 표시 포맷 개선

### 4. Carrier Detection (DCD) 구현
**현재 상태**: 미구현

**필요 작업**:
```c
// serial.c에 추가
bool serial_get_dcd(const serial_t *serial) {
    int status;
    if (ioctl(serial->fd, TIOCMGET, &status) < 0) {
        return false;
    }
    return (status & TIOCM_CAR) != 0;
}

// bridge.c 주기적 체크
if (ctx->connection_state == STATE_CONNECTED) {
    if (!serial_get_dcd(ctx->serial)) {
        MB_LOG_INFO("Carrier lost - NO CARRIER");
        modem_hangup(ctx->modem);
        bridge_handle_disconnect(ctx);
    }
}
```

**구현 항목**:
- [ ] serial_get_dcd() 함수 추가
- [ ] bridge_run() 루프에 DCD 체크
- [ ] NO CARRIER 이벤트 처리
- [ ] 재연결 로직

### 5. 속도 자동 감지 및 조정
**현재 상태**: 미구현 (고정 속도만 지원)

**필요 작업**:
```c
// CONNECT 응답 파싱
int parse_connect_speed(const char *response) {
    int speed = 0;
    // "CONNECT 57600" → 57600
    // "CONNECT 57600/ARQ" → 57600
    // "CONNECT" → default speed

    if (sscanf(response, "CONNECT %d", &speed) == 1) {
        return speed;
    }
    return DEFAULT_SPEED;
}

// 속도 변경
int adjust_to_connect_speed(bridge_t *ctx, int speed) {
    speed_t baud = speed_to_baudrate(speed);
    return serial_set_baudrate(ctx->serial, baud);
}
```

**구현 항목**:
- [ ] CONNECT 응답 파싱 로직
- [ ] 속도 → baudrate 변환 테이블
- [ ] 동적 속도 변경
- [ ] 속도 변경 후 안정화 대기

## 우선순위: 낮음 🟢

### 6. AT 명령 확장
**현재 상태**: 기본 AT 명령만 지원

**추가할 명령**:
- [ ] AT&V - 현재 설정 표시
- [ ] AT&W - 설정 저장 (가상)
- [ ] ATI0-ATI7 - 상세 정보 표시
- [ ] AT+IPR - 속도 설정
- [ ] AT+IFC - 흐름 제어 설정

### 7. S-레지스터 확장
**현재 상태**: S0-S15만 구현

**추가할 레지스터**:
- [ ] S7 - Carrier wait time
- [ ] S10 - Carrier loss delay
- [ ] S11 - DTMF duration
- [ ] S25 - DTR delay

### 8. 성능 최적화
**개선 가능 영역**:

**버퍼 크기 최적화**:
- [ ] LINE_BUFFER_SIZE 동적 조정
- [ ] Hardware message buffer 크기 튜닝
- [ ] Read buffer 크기 최적화

**CPU 사용률 개선**:
- [ ] select() timeout 동적 조정
- [ ] Busy waiting 제거
- [ ] 불필요한 문자열 연산 최소화

### 9. 리팩토링 기회
**REFACTORING.txt 섹션 8.5에서 제안된 항목**:

- [ ] 에러 처리 일관성 개선
- [ ] 로깅 레벨 세분화
- [ ] 설정 검증 강화
- [ ] 단위 테스트 추가

## 구현 순서 제안

### Phase A (1주)
1. Line Buffering 완전 통합 🔴
2. UUCP Lock 파일 완성 🔴

### Phase B (1주)
3. 시리얼 초기화 표시 🟡
4. Carrier Detection 🟡
5. 속도 자동 감지 🟡

### Phase C (선택적)
6. AT 명령 확장 🟢
7. S-레지스터 확장 🟢
8. 성능 최적화 🟢
9. 추가 리팩토링 🟢

## 테스트 요구사항

### 각 기능별 테스트
- [ ] Line buffering: 긴 메시지 분할 없이 수신
- [ ] UUCP lock: 다중 프로세스 동시 접근 차단
- [ ] Serial init: 설정 값 정확히 표시
- [ ] DCD: 연결 끊김 감지 및 복구
- [ ] Speed detection: 다양한 속도 자동 인식

### 통합 테스트
- [ ] 4시간 연속 운영
- [ ] 100회 연결/해제 반복
- [ ] 멀티바이트 문자 무결성
- [ ] 동시 다중 명령 처리

## 참고사항

### modem_sample 미적용 기능
아직 modem_sample에서 가져오지 않은 패턴:
- Baud rate probing
- Hardware flow control auto-detection
- Adaptive timeout adjustment
- Error correction negotiation

### 문서 업데이트 필요
구현 완료 시 업데이트할 문서:
- DEV_LEVEL1_RESULT.md
- INFO_USER_GUIDE.md
- INFO_TROUBLESHOOTING.md
- README.md

## 진행 상황 추적

| 작업 | 상태 | 담당 | 목표일 | 비고 |
|------|------|------|-------|------|
| Line Buffering | 🔄 진행중 | - | - | hw_msg_buffer 활용 |
| UUCP Lock | ⏸️ 대기 | - | - | 표준 규격 확인 필요 |
| Serial Init Display | ❌ 미시작 | - | - | Health check 통합 |
| DCD Detection | ❌ 미시작 | - | - | ioctl 구현 |
| Speed Detection | ❌ 미시작 | - | - | Parser 구현 |

---
*최종 업데이트: 2025-10-23*