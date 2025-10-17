# Level 1 개발 계획 - modem_sample 방식 적용

## 목표

modem_sample에서 검증된 modem 접속 처리 방식을 modembridge의 Level 1에 적용하여
실제 하드웨어 모뎀과의 안정적인 통신을 구현합니다.

## modem_sample 분석 결과

### 성공적인 통신 흐름

1. Serial port 초기화 (8N1, baudrate 설정)
2. Port locking (UUCP-style)
3. Modem 초기화 (ATZ, AT&F, AT 설정 명령)
4. Autoanswer 설정 (S0=0 또는 S0=2)
5. RING 신호 대기
6. 연결 (ATA 또는 모뎀 자동응답)
7. Carrier detect 활성화
8. 연결 속도 감지 및 baudrate 동적 조정
9. 데이터 전송 (robust write with carrier check)
10. Hangup (ATH + DTR drop)

### modem_sample의 핵심 기술

#### 1. Serial Port 설정 (serial_port.c)

```c
/* 출력 처리 활성화 - MBSE BBS 방식 */
current_tios.c_oflag = OPOST | ONLCR;  // CR -> CR-LF 변환

/* 제어 플래그 - 8N1, CLOCAL 활성화 (초기화 시 carrier 무시) */
current_tios.c_cflag |= CS8 | CREAD | HUPCL | CLOCAL;

/* 블로킹 모드 */
current_tios.c_cc[VMIN] = 1;   // 최소 1바이트 읽기
current_tios.c_cc[VTIME] = 0;  // 타임아웃 없음
```

**현재 modembridge와의 차이:**
- modembridge: `c_oflag = 0` (raw output)
- modembridge: `VMIN = 0, VTIME = 0` (non-blocking)
- modembridge: CLOCAL 없음 (초기화부터 carrier 감지)

#### 2. Port Locking

```c
/* UUCP-style lock file: /var/lock/LCK..ttyUSB0 */
int lock_port(const char *device);
void unlock_port(void);
```

**현재 modembridge:** 없음 → **추가 필요**

#### 3. AT 명령어 처리 (modem_control.c)

```c
int send_at_command(int fd, const char *command, char *response,
                    int resp_size, int timeout);
```

**핵심 기능:**
- 명령 전송 전 input flush
- 라인 단위 응답 읽기 (`serial_read_line()`)
- OK/ERROR/CONNECT/NO CARRIER 감지
- 타임아웃 처리

**현재 modembridge:**
- `modem_process_command()` - 바이트 단위 처리
- 응답 대기 메커니즘 부족

#### 4. serial_read_line() - 내부 버퍼링

```c
int serial_read_line(int fd, char *buffer, int size, int timeout)
{
    static char read_buffer[512];     /* 내부 버퍼 */
    static size_t buf_pos = 0;
    static size_t buf_len = 0;

    // 큰 청크로 읽기 (최대 128바이트)
    // 라인 종결자(\r 또는 \n) 찾을 때까지 버퍼링
    // 완전한 라인만 반환
}
```

**현재 modembridge:** 없음 → **추가 필요**

#### 5. RING 감지 및 자동응답

```c
static int wait_for_ring(int fd, int timeout, int *connected_speed)
{
    // SOFTWARE mode (S0=0): 2회 RING 후 수동 ATA
    // HARDWARE mode (S0=2): RING 감지 후 모뎀 자동응답 대기

    // serial_read_line()으로 라인 읽기
    // "RING" 문자열 감지
    // "CONNECT" 감지 시 속도 파싱
}
```

**현재 modembridge:** 기본 구현만 있음 → **강화 필요**

#### 6. Carrier Detect 관리

```c
/* 연결 후 carrier detect 활성화 */
int enable_carrier_detect(int fd)
{
    tios.c_cflag &= ~CLOCAL;  // CLOCAL 해제 -> carrier 감지
    tios.c_cflag |= CRTSCTS;  // 하드웨어 흐름 제어
}

/* Hangup 전 carrier detect 비활성화 */
tios.c_cflag |= CLOCAL;  // CLOCAL 활성화 -> carrier 무시
```

**현재 modembridge:** 초기화 시 CLOCAL 없음 → **동적 관리 필요**

#### 7. 연결 속도 감지 및 Baudrate 조정

```c
int parse_connect_speed(const char *connect_str)
{
    // "CONNECT 2400" -> 2400
    // "CONNECT 9600/V42" -> 9600
}

int adjust_serial_speed(int fd, int new_baudrate)
{
    // tcgetattr, cfsetispeed/cfsetospeed, tcsetattr
    // 실제 연결 속도에 맞춰 serial port 속도 조정
}
```

**현재 modembridge:** `serial_set_baudrate()` 있지만 자동 감지 없음 → **구현 필요**

#### 8. Robust Write (재시도 + Carrier 체크)

```c
int robust_serial_write(int fd, const char *data, int len)
{
    // 전송 전 carrier 상태 확인
    // 부분 전송 처리 (sent < len)
    // EAGAIN/EWOULDBLOCK 재시도 (최대 3회)
    // EPIPE/ECONNRESET 감지 -> ERROR_HANGUP
    // tcdrain() 대기
}
```

**현재 modembridge:** 단순 `write()` → **강화 필요**

#### 9. DTR Drop Hangup

```c
int dtr_drop_hangup(int fd)
{
    // 속도를 B0으로 설정 -> DTR 신호 drop
    // 1초 대기
    // 원래 속도로 복원
}
```

**현재 modembridge:** `serial_set_dtr()` 있지만 hangup 시 미사용 → **적용 필요**

---

## Level 1 개발 계획

### Phase 1: Serial Port 개선

#### 1-1. termios 설정 변경 (src/serial.c)

**현재 문제:**
```c
/* 현재: Raw output */
newtio.c_oflag = 0;
```

**modem_sample 방식 적용:**
```c
/* MBSE BBS 방식: 출력 처리 활성화 */
newtio.c_oflag = OPOST | ONLCR;  // CR -> CR-LF 변환
```

**변경 이유:**
- 실제 하드웨어 모뎀은 CR-LF 변환이 필요할 수 있음
- MBSE BBS에서 검증된 설정

**파일:** `src/serial.c:193`

#### 1-2. CLOCAL 동적 관리

**추가 함수:**
```c
/* include/serial.h */
int serial_enable_carrier_detect(serial_port_t *port);
int serial_disable_carrier_detect(serial_port_t *port);
int serial_check_carrier(serial_port_t *port, bool *carrier);
```

**구현 위치:** `src/serial.c`

**사용 시나리오:**
- 초기화 시: CLOCAL 활성화 (carrier 무시)
- 연결 후: CLOCAL 해제 (carrier 감지)
- Hangup 전: CLOCAL 활성화 (carrier 무시)

#### 1-3. Port Locking 추가

**추가 함수:**
```c
/* include/serial.h */
int serial_lock_port(const char *device);
void serial_unlock_port(void);
```

**구현:** UUCP-style lock file (`/var/lock/LCK..ttyUSB0`)

**파일:** `src/serial.c`

#### 1-4. serial_read_line() 추가

**함수 시그니처:**
```c
/* include/serial.h */
ssize_t serial_read_line(serial_port_t *port, char *buffer,
                         size_t size, int timeout_sec);
```

**구현 특징:**
- 내부 정적 버퍼 (512바이트)
- 큰 청크로 읽기 (128바이트)
- \r 또는 \n 감지 시 라인 반환
- 타임아웃 지원

**파일:** `src/serial.c`

#### 1-5. Robust Write 구현

**함수 시그니처:**
```c
/* include/serial.h */
ssize_t serial_write_robust(serial_port_t *port, const void *buffer,
                            size_t size);
```

**구현 내용:**
- Carrier 체크
- 부분 전송 처리
- 재시도 로직 (최대 3회, 100ms 대기)
- EPIPE/ECONNRESET 감지
- tcdrain() 대기

**파일:** `src/serial.c`

#### 1-6. DTR Drop Hangup 구현

**함수 시그니처:**
```c
/* include/serial.h */
int serial_dtr_drop_hangup(serial_port_t *port);
```

**파일:** `src/serial.c`

---

### Phase 2: Modem 제어 개선

#### 2-1. AT 명령어 응답 대기 개선 (src/modem.c)

**현재 문제:**
- `modem_process_command()`는 동기적 응답 대기 없음
- Bridge context에서 응답을 읽어야 함

**modem_sample 방식:**
```c
int modem_send_at_command(modem_t *modem, const char *command,
                          char *response, size_t resp_size,
                          int timeout_sec);
```

**구현 내용:**
- 명령 전송 전 `serial_flush(..., TCIFLUSH)`
- `serial_read_line()` 사용하여 라인 단위 읽기
- OK/ERROR/CONNECT/NO CARRIER 자동 감지
- 응답을 버퍼에 저장
- 타임아웃 처리

**파일:** `src/modem.c`

#### 2-2. 복합 명령 처리

**함수:**
```c
int modem_send_command_string(modem_t *modem, const char *cmd_string,
                              int timeout_sec);
```

**기능:**
- 세미콜론(;)으로 구분된 명령어 파싱
- 각 명령어를 순차적으로 전송
- 명령어 사이 200ms 대기

**예시:**
```c
modem_send_command_string(modem,
    "ATZ; AT&F Q0 V1 X4 &C1 &D2 S7=60 S10=120 S30=5", 5);
```

**파일:** `src/modem.c`

#### 2-3. RING 감지 및 자동응답

**함수:**
```c
int modem_wait_for_ring(modem_t *modem, int timeout_sec,
                        int *connected_speed);
```

**구현 내용:**
- SOFTWARE mode (S0=0): 2회 RING 카운트 후 반환
- HARDWARE mode (S0=2): RING 감지 후 CONNECT 대기
- `serial_read_line()` 사용
- "RING" 문자열 감지
- "CONNECT" 문자열 감지 및 속도 파싱

**파일:** `src/modem.c`

#### 2-4. ATA 명령 (수동 응답)

**함수:**
```c
int modem_answer_call(modem_t *modem, int *connected_speed);
```

**구현 내용:**
- "ATA\r" 전송
- CONNECT 응답 대기 (최대 60초)
- 연결 속도 파싱 (`parse_connect_speed()`)

**파일:** `src/modem.c`

#### 2-5. 연결 속도 파싱

**함수:**
```c
int modem_parse_connect_speed(const char *connect_str);
```

**파싱 예시:**
- "CONNECT 1200" → 1200
- "CONNECT 2400/ARQ" → 2400
- "CONNECT 9600/V42" → 9600
- "CONNECT" (속도 없음) → 300

**파일:** `src/modem.c`

#### 2-6. Hangup 개선

**함수 수정:**
```c
int modem_hangup(modem_t *modem);
```

**개선 내용:**
1. 버퍼 flush
2. Carrier detect 비활성화 (`serial_disable_carrier_detect()`)
3. ATH 명령 전송 (3초 타임아웃)
4. DTR drop (`serial_dtr_drop_hangup()`)
5. 버퍼 재flush

**파일:** `src/modem.c`

---

### Phase 3: Bridge 통합

#### 3-1. Bridge Context 수정 (include/bridge.h)

**추가 필드:**
```c
typedef struct {
    // 기존 필드들...

    /* Modem connection state */
    int connected_baudrate;      // 실제 연결 속도 (CONNECT에서 파싱)
    bool carrier_detected;       // Carrier 상태
    time_t connection_time;      // 연결 시각

    /* RING handling */
    int ring_count;              // RING 카운트
    time_t last_ring_time;       // 마지막 RING 시각
} bridge_ctx_t;
```

#### 3-2. Bridge 초기화 수정 (src/bridge.c)

**bridge_start() 수정:**
```c
int bridge_start(bridge_ctx_t *ctx)
{
    // 1. Serial port open (기존)

    // 2. Port locking 추가
    ret = serial_lock_port(ctx->config->serial_port);

    // 3. Modem 초기화 (개선된 AT 명령어 처리)
    ret = modem_send_command_string(&ctx->modem,
        "ATZ; AT&F Q0 V1 X4 &C1 &D2 S7=60 S10=120 S30=5", 5);

    // 4. Autoanswer 설정
    ret = modem_send_command_string(&ctx->modem, "ATE0 S0=2", 5);

    // 5. RING 대기 루프는 bridge_run()에서 처리
}
```

#### 3-3. Bridge 메인 루프 수정 (src/bridge.c)

**bridge_run() 수정:**
```c
int bridge_run(bridge_ctx_t *ctx)
{
    while (!interrupted) {
        // select() 대기

        // Serial port 읽기 가능
        if (FD_ISSET(serial_fd, &readfds)) {
            // Modem state에 따라 처리
            switch (ctx->modem.state) {
                case MODEM_STATE_COMMAND:
                    // AT 명령어 처리 (기존)
                    break;

                case MODEM_STATE_RINGING:
                    // RING 감지 처리
                    // modem_wait_for_ring() 또는
                    // serial_read_line() + RING 감지
                    break;

                case MODEM_STATE_ONLINE:
                    // Carrier 체크
                    serial_check_carrier(&ctx->serial, &carrier);
                    if (!carrier) {
                        // Carrier lost -> 연결 종료
                        modem_hangup(&ctx->modem);
                    }

                    // 데이터 전송 (기존)
                    break;
            }
        }

        // Telnet 읽기 가능 (Level 2 - 현재는 제외)
    }
}
```

#### 3-4. 연결 확립 시 Carrier Detect 활성화

**bridge_connect() 또는 ATA 성공 후:**
```c
// 1. Carrier detect 활성화
serial_enable_carrier_detect(&ctx->serial);

// 2. 연결 속도가 다르면 baudrate 조정
if (connected_speed > 0 && connected_speed != configured_baudrate) {
    speed_t new_speed = baudrate_to_speed_t(connected_speed);
    serial_set_baudrate(&ctx->serial, new_speed);
}

// 3. 상태 전환
ctx->modem.state = MODEM_STATE_ONLINE;
ctx->connected_baudrate = connected_speed;
ctx->carrier_detected = true;
ctx->connection_time = time(NULL);
```

#### 3-5. 데이터 전송 시 Robust Write 사용

**기존:**
```c
serial_write(&ctx->serial, buffer, len);
```

**개선:**
```c
ssize_t sent = serial_write_robust(&ctx->serial, buffer, len);
if (sent < 0) {
    if (sent == ERROR_HANGUP) {
        // Carrier lost
        MB_LOG_WARNING("Carrier lost during transmission");
        modem_hangup(&ctx->modem);
        // 연결 재시도 또는 대기
    }
}
```

---

## 구현 순서

### Step 1: Serial Port 기능 추가 (1-2일)
- [ ] `serial_read_line()` 구현 및 테스트
- [ ] Port locking 추가
- [ ] `serial_enable/disable_carrier_detect()` 구현
- [ ] `serial_check_carrier()` 구현
- [ ] `serial_write_robust()` 구현
- [ ] `serial_dtr_drop_hangup()` 구현
- [ ] termios 출력 플래그 변경 (OPOST | ONLCR)

### Step 2: Modem 제어 강화 (1-2일)
- [ ] `modem_send_at_command()` 구현 (응답 대기 포함)
- [ ] `modem_send_command_string()` 구현 (복합 명령)
- [ ] `modem_parse_connect_speed()` 구현
- [ ] `modem_wait_for_ring()` 구현
- [ ] `modem_answer_call()` 구현
- [ ] `modem_hangup()` 개선 (DTR drop 추가)

### Step 3: Bridge 통합 (1일)
- [ ] Bridge context에 필드 추가
- [ ] `bridge_start()` 수정 (port locking, 개선된 초기화)
- [ ] `bridge_run()` 수정 (RING 감지, carrier 체크)
- [ ] 연결 확립 시 carrier detect 활성화 및 baudrate 조정
- [ ] 데이터 전송 시 robust write 사용

### Step 4: 테스트 (1일)
- [ ] Serial port 기본 동작 테스트
- [ ] AT 명령어 응답 테스트
- [ ] RING 감지 테스트
- [ ] 연결/해제 테스트
- [ ] Baudrate 자동 조정 테스트
- [ ] Carrier lost 감지 테스트

---

## 테스트 계획

### 1. Serial Port 테스트

**테스트 케이스:**
- socat으로 가상 serial port 생성
- minicom으로 AT 명령 전송
- 응답 확인

```bash
# Virtual serial port pair
socat -d -d pty,raw,echo=0 pty,raw,echo=0

# Terminal 1: modembridge
./build/modembridge -c modembridge.conf -v

# Terminal 2: minicom
minicom -D /dev/pts/X
```

### 2. 실제 모뎀 테스트

**테스트 시나리오:**
1. 모뎀 초기화 (ATZ, AT&F, 설정 명령)
2. Autoanswer 설정 (S0=2)
3. RING 신호 대기
4. 자동 응답 또는 수동 ATA
5. CONNECT 확인 및 속도 파싱
6. Carrier detect 활성화 확인
7. 데이터 전송 (첫 번째, 두 번째 메시지)
8. Carrier 상태 모니터링
9. Hangup (ATH + DTR drop)

**검증 항목:**
- [ ] AT 명령어 정상 응답
- [ ] RING 감지 정확성
- [ ] 연결 속도 정확한 파싱
- [ ] Baudrate 자동 조정 성공
- [ ] Carrier detect 정상 동작
- [ ] 데이터 전송 성공
- [ ] Hangup 정상 동작

---

## 예상 결과

modem_sample의 검증된 방식을 적용하여:

1. **안정적인 Serial Port 통신**
   - MBSE BBS 스타일의 termios 설정
   - Port locking으로 다중 접근 방지
   - 내부 버퍼링으로 라인 단위 읽기

2. **신뢰성 있는 AT 명령어 처리**
   - 동기적 응답 대기
   - 타임아웃 처리
   - 복합 명령 지원

3. **정확한 RING 감지 및 응답**
   - 2회 RING 카운팅
   - SOFTWARE/HARDWARE mode 지원
   - 연결 속도 자동 감지

4. **동적 Carrier 관리**
   - 초기화 시 carrier 무시
   - 연결 후 carrier 감지 활성화
   - Hangup 전 carrier 무시

5. **안정적인 데이터 전송**
   - Carrier 체크
   - 재시도 로직
   - 부분 전송 처리

6. **깔끔한 연결 종료**
   - ATH 명령
   - DTR drop
   - Port unlock

---

## 참고 자료

- `../modem_sample/` - 검증된 구현
- MBSE BBS mbcico 소스 코드
- `process_modem_connection.txt` - 통신 흐름 분석
- `SERIAL_INIT_PLAN.md` - 기존 serial 초기화 계획
- `MODEM_COMMAND_PLAN.md` - 기존 모뎀 명령 계획

---

## 다음 단계 (Level 2, Level 3)

Level 1 완료 후:
- **Level 2**: Telnet 서버 연결 및 데이터 수신
- **Level 3**: Serial ↔ Telnet 데이터 동기화 (Thread 간 통신)

Level 1이 안정화되면 Level 2, Level 3를 순차적으로 진행합니다.
