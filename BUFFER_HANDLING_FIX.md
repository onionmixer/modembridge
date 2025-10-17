# Modem Buffer 처리 개선 (Hardware Message Buffer)

## 문제 발견

사용자 보고 로그:
```
[INFO] Ring count: 2, Auto-answer setting (S0): 0
[INFO] === SOFTWARE AUTO-ANSWER MODE (S0=0) ===
[INFO] Ring threshold reached: 2/2 rings - sending ATA to hardware modem
[INFO] ATA command sent to hardware modem (5 bytes) - waiting for CONNECT response
[INFO] Modem state: CONNECTING (software-initiated)

[INFO] Draining initialization responses (3 bytes): [
C]
[INFO] Draining initialization responses (17 bytes): [ONNECT 2400/ARQ
]
```

## 근본 원인

### 문제 1: CONNECT 응답이 분할 수신됨
- ATA 명령 전송 후 CONNECT 응답이 두 조각으로 수신됨:
  1. `"\r\nC"` (3 bytes)
  2. `"ONNECT 2400/ARQ\r\n"` (17 bytes)

### 문제 2: Draining 단계에서 RING만 확인
수정 전 코드 (bridge.c:815-823):
```c
/* Check for RING even during drain phase */
if (strstr((char *)drain_buf, "RING") != NULL) {
    modem_process_hardware_message(&ctx->modem, (char *)drain_buf, drained);
}
```

**문제점:**
- RING만 확인하고 CONNECT, NO CARRIER 등은 무시
- ATA 전송 후 CONNECT가 draining 단계에서 수신되는데 처리 안 됨
- CONNECT 메시지가 손실됨

## modem_sample 패턴 분석

### modem_sample의 buffer 처리 (serial_port.c:239-375)

```c
int serial_read_line(int fd, char *buffer, int size, int timeout)
{
    static char read_buffer[512];     /* Internal buffer */
    static size_t buf_pos = 0;
    static size_t buf_len = 0;

    while (1) {
        /* Check for complete line in buffer */
        for (i = buf_pos; i < buf_len; i++) {
            if (c == '\n' || c == '\r') {
                /* Found line - return it */
                return line;
            }
        }

        /* No complete line - read more data */
        rc = serial_read(fd, chunk, sizeof(chunk), timeout);
        if (rc > 0) {
            /* Append to buffer */
            memcpy(&read_buffer[buf_len], chunk, rc);
            buf_len += rc;
        }
    }
}
```

**핵심 특징:**
1. **내부 버퍼에 데이터 누적** - 여러 read() 호출에 걸쳐 데이터 유지
2. **완전한 라인을 받을 때까지 대기** - CR/LF를 찾을 때까지 계속 읽음
3. **Fragment 처리** - "CONN" + "ECT\r\n" 같은 분할 수신 자동 처리

### modem_sample의 초기화 후 처리 (modem_sample.c:240-268)

```c
/* STEP 7: Monitor serial port for RING signal */
print_message("SOFTWARE mode: Waiting for RING signals...");
rc = wait_for_ring(serial_fd, RING_WAIT_TIMEOUT, NULL);

if (rc != SUCCESS) {
    print_error("Failed to detect RING signal");
    goto cleanup;
}

/* STEP 8: Answer the call with speed detection */
print_message("Answering incoming call (ATA) with speed detection...");
rc = modem_answer_with_speed_adjust(serial_fd, &connected_speed);
```

**핵심 차이점:**
- **초기화 완료 후 바로 wait_for_ring() 시작** - draining 없음
- **ATA 전송 후 즉시 CONNECT 대기** - modem_answer_with_speed_adjust()
- **응답을 놓치지 않음** - 각 단계에서 명확하게 응답 수신

## modembridge의 Hardware Message Buffer

### 현재 구현 (modem.c:838-1057)

```c
bool modem_process_hardware_message(modem_t *modem, const char *data, size_t len)
{
    /* Append to internal buffer */
    memcpy(modem->hw_msg_buffer + modem->hw_msg_len, data, copy_len);
    modem->hw_msg_len += copy_len;

    /* Check for RING */
    if (strstr(buffer, "RING") != NULL) {
        /* Process RING */
    }

    /* Check for CONNECT */
    else if (strstr(buffer, "CONNECT") != NULL) {
        /* Wait for line ending */
        if (line_end) {
            /* Complete CONNECT message */
            modem->state = MODEM_STATE_ONLINE;
            modem->carrier = true;
        } else {
            /* Partial - wait for more data */
        }
    }
}
```

**특징:**
- ✅ 내부 버퍼에 데이터 누적 (hw_msg_buffer)
- ✅ 완전한 메시지를 받을 때까지 대기
- ✅ Fragment 자동 처리
- ✅ 20초 timeout

**이미 잘 구현되어 있음!**

## 수정 내용

### bridge.c:800-827 - Draining 단계 개선

**수정 전:**
```c
/* Check for RING even during drain phase */
if (strstr((char *)drain_buf, "RING") != NULL) {
    modem_process_hardware_message(&ctx->modem, (char *)drain_buf, drained);
}
```

**수정 후:**
```c
/* Process ALL hardware modem messages during drain phase (modem_sample pattern) */
/* This handles RING, CONNECT, NO CARRIER that may arrive during initialization */
bool msg_handled = modem_process_hardware_message(&ctx->modem, (char *)drain_buf, drained);

if (msg_handled) {
    printf("[INFO] Hardware message processed during drain phase\n");
    MB_LOG_INFO("Hardware message processed during drain phase");
}
```

**변경 사항:**
1. ✅ **모든 데이터**를 `modem_process_hardware_message()`로 전달
2. ✅ RING뿐만 아니라 **CONNECT, NO CARRIER** 등도 처리
3. ✅ 처리 여부를 로그로 확인

## 동작 방식

### Fragment 처리 예시

**첫 번째 read:**
```
Buffer: "\r\nC"
hw_msg_buffer: "\r\nC"
strstr("CONNECT"): NOT FOUND
→ Buffer에 유지, hardware_msg_detected = false
```

**두 번째 read:**
```
Buffer: "ONNECT 2400/ARQ\r\n"
hw_msg_buffer: "\r\nCONNECT 2400/ARQ\r\n"
strstr("CONNECT"): FOUND!
Line ending check: YES (\r\n 있음)
→ CONNECT 처리, state = ONLINE, hardware_msg_detected = true
```

### 예상 로그 출력

```
[INFO] Draining initialization responses (3 bytes): [
C]
[INFO] Draining initialization responses (17 bytes): [ONNECT 2400/ARQ
]
[INFO] Hardware message processed during drain phase
[INFO] *** CONNECT detected from hardware modem ***
[INFO] Full message: [
CONNECT 2400/ARQ
]
[INFO] Connection speed: 2400 baud
[INFO] Modem state changed: ONLINE=true, carrier=true
```

## modem_sample과의 비교

### modem_sample 방식
```
1. init_modem() - 초기화 명령 전송 및 응답 수신
2. set_modem_autoanswer() - S0 설정 및 응답 수신
3. wait_for_ring() - RING 대기 (serial_read_line)
4. modem_answer_with_speed_adjust() - ATA 전송 및 CONNECT 대기 (serial_read_line)
5. 연결 완료
```

### modembridge 방식 (현재)
```
1. MODEM_INIT_COMMAND 전송 및 응답 수신 (1회 read)
2. MODEM_AUTOANSWER_COMMAND 전송 및 응답 수신 (1회 read)
3. Buffer draining (10회 read, 100ms 간격) ← 여기서 RING/CONNECT 처리
4. Thread 시작
5. Thread에서 serial data 모니터링
```

**주요 차이점:**
- modem_sample: 각 단계에서 명확하게 응답 대기
- modembridge: Draining 단계에서 일괄 처리 + Thread에서 지속 모니터링

**장점:**
- ✅ Thread 기반으로 더 반응적
- ✅ 초기화와 모니터링 분리

**단점 (수정 전):**
- ❌ Draining에서 RING만 확인 → **수정 완료**
- ✅ 이제 모든 hardware message 처리

## 추가 개선 사항 (향후)

### 1. 초기화 응답 수신 개선
현재는 AT 명령 후 1회만 read하는데, OK/ERROR를 받을 때까지 읽어야 함:

```c
// 현재:
serial_write(&ctx->serial, cmd_buf, strlen(cmd_buf));
usleep(500000);
serial_read(&ctx->serial, response, sizeof(response));  // 1회만

// 개선:
serial_write(&ctx->serial, cmd_buf, strlen(cmd_buf));
while (timeout not reached) {
    serial_read_line(&ctx->serial, line, sizeof(line), 1);
    if (strstr(line, "OK") || strstr(line, "ERROR")) {
        break;
    }
}
```

### 2. Draining Timeout 최적화
현재: 10회 * 100ms = 1초
개선: 더 짧게 (예: 5회 * 50ms = 250ms)

### 3. Debug Logging 강화
Hardware message buffer 상태를 더 자세히 로그

## 테스트 체크리스트

- [x] Build 성공
- [ ] RING 2회 감지 후 ATA 전송
- [ ] CONNECT fragment 수신 처리 (C + ONNECT)
- [ ] CONNECT 처리 후 ONLINE 상태 전환
- [ ] Connection 속도 파싱 (2400 baud)
- [ ] Timestamp 전송 시작 (Level 1)

## 관련 파일

- `src/bridge.c:800-827` - Draining 단계 수정
- `src/modem.c:838-1057` - Hardware message buffer 처리 (변경 없음, 이미 잘 구현됨)
- `../modem_sample/serial_port.c:239-375` - serial_read_line() 참고
- `../modem_sample/modem_sample.c:240-268` - 초기화 후 처리 참고

## 참고

modem_process_hardware_message()의 내부 버퍼 처리가 이미 modem_sample의 serial_read_line() 패턴과 유사하게 구현되어 있습니다:
- ✅ Static buffer에 데이터 누적
- ✅ 완전한 메시지를 찾을 때까지 대기
- ✅ Line ending 확인
- ✅ 20초 timeout

따라서 현재 수정(draining에서 모든 message 처리)만으로 충분할 것으로 예상됩니다.
