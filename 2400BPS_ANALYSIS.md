# 2400bps 모뎀 환경에서의 코드 대응 분석

## 1. 2400bps 전송 속도 특성

### 기본 계산
```
- Baudrate: 2400 bps
- 프레임: 8N1 (8 data bits + 1 start bit + 1 stop bit = 10 bits/character)
- 실제 처리량: 2400 / 10 = 240 characters/second
- 1 character 전송 시간: 1000ms / 240 = ~4.17ms
- 10 bytes 전송 시간: 4.17ms × 10 = ~41.7ms
- 100ms 동안 전송 가능: 240 × 0.1 = 24 bytes
```

### 실제 모뎀 메시지 전송 시간
```
"RING\r\n"              = 6 bytes  → ~25ms
"CONNECT 2400\r\n"      = 14 bytes → ~58ms
"CONNECT 2400/ARQ\r\n"  = 17 bytes → ~71ms
"NO CARRIER\r\n"        = 12 bytes → ~50ms
```

## 2. 현재 코드의 버퍼 및 타이밍 설정

### 버퍼 크기 (include/common.h:34-36)
```c
#define BUFFER_SIZE         4096    // 메인 I/O 버퍼
#define SMALL_BUFFER_SIZE   256     // 응답 버퍼
#define LINE_BUFFER_SIZE    1024    // 명령 버퍼
```

**분석:**
- ✅ 4096 bytes는 2400bps에서 충분함 (17초분의 데이터)
- ✅ Hardware message buffer (LINE_BUFFER_SIZE=1024)도 충분

### Serial 포트 설정 (src/serial.c:199-200)
```c
newtio.c_cc[VMIN] = 0;   /* Non-blocking read */
newtio.c_cc[VTIME] = 0;  /* No timeout */
```

**분석:**
- ✅ Non-blocking 모드로 설정됨
- ✅ serial_read()는 즉시 반환 (사용 가능한 만큼만 읽음)
- ⚠️ 부분 메시지를 여러 번의 read()로 수신할 가능성 높음

### 스레드 처리 주기 (src/bridge.c:1668)
```c
usleep(100000);  /* 100ms - balanced between responsiveness and CPU usage */
```

**분석:**
- ✅ 100ms 주기는 2400bps에 적절 (24 bytes/cycle 처리)
- ✅ CPU 사용률과 응답성 사이의 균형
- ⚠️ 하지만 메시지가 부분적으로 도착할 수 있음

## 3. 부분 메시지 처리 메커니즘

### Hardware Message Buffer (src/modem.c:818-833)
```c
/* Check for timeout - clear buffer if data is too old (20 seconds) */
if (modem->hw_msg_len > 0 && (now - modem->hw_msg_last_time) > 20) {
    MB_LOG_DEBUG("Hardware message buffer timeout - clearing old data");
    modem->hw_msg_len = 0;
    memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
}

/* Append new data to hardware message buffer */
size_t space_left = sizeof(modem->hw_msg_buffer) - modem->hw_msg_len - 1;
size_t copy_len = MIN(len, space_left);
if (copy_len > 0) {
    memcpy(modem->hw_msg_buffer + modem->hw_msg_len, data, copy_len);
    modem->hw_msg_len += copy_len;
    modem->hw_msg_buffer[modem->hw_msg_len] = '\0';
    modem->hw_msg_last_time = now;
}
```

**분석:**
- ✅ **부분 메시지 버퍼링 구현됨**: 여러 serial_read() 호출에 걸쳐 수신된 데이터를 축적
- ✅ **20초 타임아웃**: 불완전한 메시지를 정리 (네트워크 지연 고려, 매우 안전)
- ✅ **Timestamp 갱신**: 새 데이터 도착 시 타임아웃 리셋

### 시나리오 예시: "CONNECT 2400/ARQ\r\n" 수신

**2400bps 환경에서의 실제 수신 과정:**

```
Time    | serial_read() | Data Received      | Buffer State
--------+---------------+-------------------+---------------------
T+0ms   | Call 1        | "\r\nCON"         | "\r\nCON"
T+100ms | Call 2        | "NECT "           | "\r\nCONNECT "
T+200ms | Call 3        | "2400/A"          | "\r\nCONNECT 2400/A"
T+300ms | Call 4        | "RQ\r\n"          | "\r\nCONNECT 2400/ARQ\r\n" ✓ DETECTED
```

**코드 동작:**
1. Call 1: `strstr(buffer, "CONNECT")` → NULL (부분 메시지)
2. Call 2: `strstr(buffer, "CONNECT")` → FOUND, but no `\r` → 대기
3. Call 3: `strstr(buffer, "CONNECT")` → FOUND, but no `\r` → 대기
4. Call 4: `strstr(buffer, "CONNECT")` → FOUND, `\r` 발견 → **처리 완료**

### 불완전 메시지 보호 (src/modem.c:933-952)
```c
/* If we have a line ending at the end of buffer, check if it's a complete message */
else if (modem->hw_msg_len > 0) {
    size_t len = strlen(buffer);
    if (len >= 2 && buffer[len-2] == '\r' && buffer[len-1] == '\n') {
        /* Check if this might be start of CONNECT or other message */
        if (len <= 3 || /* Very short, might be start of message like "\r\nC" */
            strstr(buffer, "CON") != NULL || /* Partial CONNECT */
            strstr(buffer, "RIN") != NULL || /* Partial RING */
            strstr(buffer, "NO ") != NULL) { /* Partial NO CARRIER */
            /* Keep buffer, might be incomplete message */
            MB_LOG_DEBUG("Keeping partial message in buffer: [%s]", buffer);
        } else {
            /* Complete line but unrecognized - clear buffer */
            MB_LOG_DEBUG("Clearing unrecognized complete message: [%s]", buffer);
            modem->hw_msg_len = 0;
            memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
        }
    }
}
```

**분석:**
- ✅ **부분 메시지 유지**: "CON", "RIN", "NO " 같은 시작 부분을 감지하고 버퍼 유지
- ✅ **타임아웃 보호**: 2초 동안 완성되지 않으면 자동 정리

## 4. AT 명령 응답 대기 시간

### Modem Initialization (src/bridge.c:636, 725)
```c
usleep(200000);  /* 200ms between commands */
usleep(500000);  /* 500ms for response */
```

**분석:**
- ⚠️ **200ms는 부족할 수 있음**:
  - 2400bps에서 명령 전송: "ATZ\r\n" (5 bytes) = ~21ms
  - 모뎀 처리 시간: ~50-100ms (하드웨어 의존)
  - 응답 수신: "OK\r\n" (4 bytes) = ~17ms
  - **총 필요 시간: ~90-140ms**
  - 200ms는 충분하지만 여유가 적음

- ✅ **500ms는 충분함**: 대부분의 AT 명령 응답에 적합

### Buffer Drain (src/bridge.c:772-781)
```c
do {
    drained = serial_read(&ctx->serial, drain_buf, sizeof(drain_buf));
    if (drained > 0) {
        printf("[INFO] Drained %zd bytes of initialization responses\n", drained);
        MB_LOG_DEBUG("Drained %zd bytes: [%.*s]", drained, (int)drained, drain_buf);
        drain_attempts++;
    }
    usleep(100000);  /* 100ms between drain attempts */
} while (drained > 0 && drain_attempts < 10);
```

**분석:**
- ✅ **100ms 간격은 적절**: 2400bps에서 24 bytes씩 드레인 가능
- ✅ **10회 시도**: 최대 240 bytes 드레인 가능 (충분함)

## 5. Timestamp 전송과 버퍼 오버플로우

### Timestamp 크기 (src/bridge.c:1525-1532)
```c
char timestamp_msg[128];
int len = snprintf(timestamp_msg, sizeof(timestamp_msg),
                  "\r\n[%04d-%02d-%02d %02d:%02d:%02d] Level 1 Active\r\n",
                  tm_info->tm_year + 1900,
                  tm_info->tm_mon + 1,
                  tm_info->tm_mday,
                  tm_info->tm_hour,
                  tm_info->tm_min,
                  tm_info->tm_sec);
// 실제 크기: ~43 bytes
```

**2400bps에서의 전송 시간:**
```
43 bytes × 4.17ms = ~179ms
```

### tcdrain() 사용 (src/serial.c:355)
```c
ssize_t sent = serial_write(&ctx->serial, (unsigned char *)timestamp_msg, len);

// serial_write() 내부:
n = write(port->fd, buffer, size);
if (n < 0) { /* error handling */ }

/* Wait for data to be transmitted */
tcdrain(port->fd);  // ← 중요!

return n;
```

**분석:**
- ✅ **tcdrain() 사용**: 데이터 전송 완료까지 대기
- ✅ **버퍼 오버플로우 방지**: 다음 전송 전에 이전 전송 완료 보장
- ✅ **2400bps에서도 안전**: 179ms 전송 시간은 10초 간격 대비 충분히 짧음

## 6. 잠재적 문제점

### 6.1 CONNECT 메시지 완성도 체크 (src/modem.c:878-886)
```c
else if (strstr(buffer, "CONNECT") != NULL) {
    char *connect_pos = strstr(buffer, "CONNECT");
    if (connect_pos != NULL) {
        /* Check if message looks complete (has line ending or next word) */
        char *line_end = strstr(connect_pos, "\r");
        if (!line_end) line_end = strstr(connect_pos, "\n");

        if (line_end || strlen(connect_pos) >= 7) { /* At least "CONNECT" */
            // 처리...
        }
    }
}
```

**문제점:**
- ⚠️ `strlen(connect_pos) >= 7` 조건이 약함
- "CONNECT 2400" (12 bytes)를 받아야 하는데 "CONNECT" (7 bytes)만 와도 처리 가능
- 2400bps에서는 "CONNECT"까지만 도착할 수 있음 (첫 29ms)

**권장 수정:**
```c
/* Wait for complete line ending */
if (line_end) {  // Only process if \r or \n found
    // 처리...
}
// Remove: || strlen(connect_pos) >= 7
```

### 6.2 AT Command Response Timing

**현재 코드 (src/bridge.c:636):**
```c
usleep(200000);  /* 200ms between commands */
```

**권장 수정:**
```c
/* Adjust delay based on baudrate */
int delay_ms = (ctx->config->baudrate_value <= 2400) ? 300 : 200;
usleep(delay_ms * 1000);
```

### 6.3 Hardware Message Timeout

**현재 코드 (src/modem.c:819):**
```c
if (modem->hw_msg_len > 0 && (now - modem->hw_msg_last_time) > 20) {
```

**분석:**
- ✅ 20초는 매우 충분함
- 2400bps에서 가장 긴 메시지 (~20 bytes)도 100ms 이내 도착
- 20초는 네트워크 지연, 시스템 부하 등을 충분히 고려한 안전 마진

## 7. 성능 최적화 권장사항

### 7.1 Baudrate-Adaptive Timing
```c
/* Calculate appropriate delays based on baudrate */
int get_command_delay_ms(int baudrate) {
    if (baudrate <= 1200) return 500;   /* Very slow */
    if (baudrate <= 2400) return 300;   /* Slow */
    if (baudrate <= 9600) return 200;   /* Medium */
    return 100;                          /* Fast */
}
```

### 7.2 Buffer Monitoring
```c
/* Log buffer usage for debugging */
if (modem->hw_msg_len > 0) {
    MB_LOG_DEBUG("Hardware buffer: %zu/%d bytes (age: %ld sec)",
                 modem->hw_msg_len,
                 LINE_BUFFER_SIZE,
                 now - modem->hw_msg_last_time);
}
```

### 7.3 CONNECT Message Validation Improvement
```c
/* Only process CONNECT when line is complete */
else if (strstr(buffer, "CONNECT") != NULL) {
    char *connect_pos = strstr(buffer, "CONNECT");
    char *line_end = strstr(connect_pos, "\r");
    if (!line_end) line_end = strstr(connect_pos, "\n");

    if (line_end) {  /* Wait for complete line */
        // Extract and process complete CONNECT message
        MB_LOG_INFO("*** CONNECT detected from hardware modem ***");
        // ...
    } else {
        MB_LOG_DEBUG("Partial CONNECT message - waiting for completion");
    }
}
```

## 8. 종합 평가

### 강점 (✅)
1. **부분 메시지 버퍼링 구현됨**: hardware message buffer가 여러 serial_read()에 걸쳐 데이터 축적
2. **적절한 타임아웃**: 2초 타임아웃으로 불완전한 메시지 정리
3. **tcdrain() 사용**: 전송 버퍼 오버플로우 방지
4. **충분한 버퍼 크기**: 4096 bytes는 2400bps에서 17초분의 데이터
5. **Non-blocking I/O**: 응답성 유지

### 약점 (⚠️)
1. **CONNECT 메시지 완성도 체크 약함**: `strlen(connect_pos) >= 7` 조건이 부분 메시지 처리 유발 가능
2. **고정된 AT 명령 대기 시간**: baudrate에 따른 동적 조정 없음
3. **디버깅 정보 부족**: 부분 메시지 도착 시 로깅 부족

### 권장 수정사항
1. **CONNECT 메시지 처리 개선**: line ending 필수 확인
2. **Baudrate-adaptive timing**: AT 명령 대기 시간 동적 조정
3. **디버깅 로그 추가**: 부분 메시지 추적용 로그

## 9. 결론

**현재 코드는 2400bps 환경에서 대체로 안정적으로 동작할 것으로 예상됩니다.**

주요 메커니즘이 잘 구현되어 있음:
- ✅ 부분 메시지 버퍼링
- ✅ 적절한 타임아웃
- ✅ Non-blocking I/O
- ✅ tcdrain()을 통한 전송 완료 보장

다만, CONNECT 메시지 처리 부분의 완성도 체크를 강화하면 더욱 안정적인 동작을 보장할 수 있습니다.

**테스트 시나리오:**
1. 2400bps 모뎀 연결
2. RING → CONNECT 2400/ARQ 수신 확인
3. Timestamp 전송 안정성 확인 (10초 간격)
4. 연속 데이터 전송 시 손실 없음 확인
