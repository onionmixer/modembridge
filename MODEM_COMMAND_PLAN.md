# MODEM_COMMAND 기능 추가 구현 계획

## 요구사항 요약

1. `modembridge.conf`에 `MODEM_COMMAND` 항목 추가
2. Health check 시 modem에 AT 명령을 보내고 응답 확인 및 출력
3. AT 응답과 관계없이 `MODEM_COMMAND`에 지정된 명령들을 modem에 전송
4. 전송한 명령과 modem의 응답을 표준 출력으로 출력
5. Health check가 재실행되면 위 과정도 재실행

## MODEM_COMMAND 문자열 규칙

- `;` 문자로 여러 AT 명령을 구분
- 예: `"AT&C1 B0 X3; ATS0=1H0"` → 2개의 명령
  - 명령 1: `AT&C1 B0 X3`
  - 명령 2: `ATS0=1H0`
- modem에 전송할 때는 `;`를 제거하고 각 명령에 `\r\n` 추가

## 구현 단계

### [1] config.h 수정 - MODEM_COMMAND 필드 추가

**파일**: `include/config.h`

**위치**: `config_t` 구조체 (라인 29-52)

**추가할 내용**:
```c
typedef struct {
    /* Serial port settings */
    char serial_port[SMALL_BUFFER_SIZE];
    speed_t baudrate;
    int baudrate_value;
    parity_t parity;
    int data_bits;
    int stop_bits;
    flow_control_t flow_control;
    char modem_command[LINE_BUFFER_SIZE];  // ← 추가

    /* Telnet settings */
    char telnet_host[SMALL_BUFFER_SIZE];
    int telnet_port;

    /* Runtime options */
    bool daemon_mode;
    char pid_file[SMALL_BUFFER_SIZE];
    int log_level;

    /* Data logging options */
    bool data_log_enabled;
    char data_log_file[SMALL_BUFFER_SIZE];
} config_t;
```

**이유**: 설정 파일에서 읽은 `MODEM_COMMAND` 문자열을 저장할 공간 필요

---

### [2] config.c 수정 - 기본값 초기화

**파일**: `src/config.c`

**위치**: `config_init()` 함수 (라인 29-60)

**수정 전**:
```c
void config_init(config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(config_t));

    /* Default serial port settings */
    SAFE_STRNCPY(cfg->serial_port, "/dev/ttyUSB0", sizeof(cfg->serial_port));
    cfg->baudrate = B57600;
    cfg->baudrate_value = 57600;
    cfg->parity = PARITY_NONE;
    cfg->data_bits = 8;
    cfg->stop_bits = 1;
    cfg->flow_control = FLOW_RTSCTS;
    // ← 여기에 추가

    /* Default telnet settings */
    SAFE_STRNCPY(cfg->telnet_host, "127.0.0.1", sizeof(cfg->telnet_host));
    cfg->telnet_port = 23;
    // ...
}
```

**수정 후**:
```c
void config_init(config_t *cfg)
{
    // ... (기존 코드)

    cfg->flow_control = FLOW_RTSCTS;
    cfg->modem_command[0] = '\0';  // ← 추가 (빈 문자열로 초기화)

    /* Default telnet settings */
    // ...
}
```

---

### [3] config.c 수정 - MODEM_COMMAND 파싱

**파일**: `src/config.c`

**위치**: `parse_config_line()` 함수 (라인 65-154)

**수정 전**:
```c
    else if (strcasecmp(key, "FLOW") == 0) {
        cfg->flow_control = config_str_to_flow(value);
    }
    else if (strcasecmp(key, "TELNET_HOST") == 0) {
        SAFE_STRNCPY(cfg->telnet_host, value, sizeof(cfg->telnet_host));
    }
```

**수정 후**:
```c
    else if (strcasecmp(key, "FLOW") == 0) {
        cfg->flow_control = config_str_to_flow(value);
    }
    else if (strcasecmp(key, "MODEM_COMMAND") == 0) {  // ← 추가
        SAFE_STRNCPY(cfg->modem_command, value, sizeof(cfg->modem_command));
        MB_LOG_DEBUG("Modem command configured: %s", cfg->modem_command);
    }
    else if (strcasecmp(key, "TELNET_HOST") == 0) {
        SAFE_STRNCPY(cfg->telnet_host, value, sizeof(cfg->telnet_host));
    }
```

---

### [4] healthcheck.h 수정 - 함수 시그니처 변경

**파일**: `include/healthcheck.h`

**위치**: 라인 54-61

**수정 전**:
```c
/**
 * Check modem device responsiveness (optional, with timeout)
 * @param device Serial device path
 * @param cfg Configuration (for baudrate, etc.)
 * @param result Output result structure
 * @return SUCCESS if check completed, error code otherwise
 */
int healthcheck_modem_device(const char *device, const config_t *cfg,
                             health_check_result_t *result);
```

**수정 후**: (변경 없음, 이미 `cfg` 전체를 받으므로 `modem_command` 접근 가능)

---

### [5] healthcheck.c 수정 - Modem 체크 로직 개선

**파일**: `src/healthcheck.c`

**위치**: `healthcheck_modem_device()` 함수 (라인 117-187)

**현재 동작**:
1. Serial port 열기
2. AT 명령 전송
3. 2초 대기하며 응답 확인
4. Serial port 닫기

**새로운 동작**:
1. Serial port 열기
2. **AT 명령 전송하고 응답 출력** ← 추가
3. **MODEM_COMMAND 파싱**
4. **각 명령을 순차적으로 전송하고 응답 출력** ← 추가
5. Serial port 닫기

**구현 코드** (전체 함수 재작성):

```c
/**
 * Helper: Send command to modem and read response with timeout
 * Returns number of bytes read, or -1 on error
 */
static ssize_t send_at_command_and_wait(serial_port_t *port,
                                         const char *command,
                                         unsigned char *response,
                                         size_t response_size,
                                         int timeout_sec)
{
    char cmd_buf[SMALL_BUFFER_SIZE];
    fd_set readfds;
    struct timeval tv;
    int ret;

    /* Format command with CR+LF */
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", command);

    /* Send command */
    ssize_t written = serial_write(port, (const unsigned char *)cmd_buf,
                                   strlen(cmd_buf));
    if (written < 0) {
        return -1;
    }

    /* Wait for response */
    FD_ZERO(&readfds);
    FD_SET(port->fd, &readfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ret = select(port->fd + 1, &readfds, NULL, NULL, &tv);

    if (ret > 0) {
        /* Data available */
        ssize_t n = serial_read(port, response, response_size - 1);
        if (n > 0) {
            response[n] = '\0';  /* Null-terminate */
            return n;
        }
    } else if (ret == 0) {
        /* Timeout */
        return 0;
    }

    return -1;
}

/**
 * Helper: Parse semicolon-separated commands
 * Modifies input string in place
 * Returns array of command pointers (caller must free)
 */
static int parse_modem_commands(char *modem_command_str,
                                char **commands,
                                int max_commands)
{
    int count = 0;
    char *token;
    char *saveptr;

    if (modem_command_str == NULL || modem_command_str[0] == '\0') {
        return 0;
    }

    /* Split by semicolon */
    token = strtok_r(modem_command_str, ";", &saveptr);
    while (token != NULL && count < max_commands) {
        /* Trim whitespace */
        token = trim_whitespace(token);

        if (strlen(token) > 0) {
            commands[count++] = token;
        }

        token = strtok_r(NULL, ";", &saveptr);
    }

    return count;
}

/**
 * Check modem device responsiveness
 * Sends AT command and waits for response (2 second timeout)
 * Then sends MODEM_COMMAND commands if configured
 */
int healthcheck_modem_device(const char *device, const config_t *cfg,
                             health_check_result_t *result)
{
    serial_port_t port;
    unsigned char response[SMALL_BUFFER_SIZE];
    char modem_cmd_copy[LINE_BUFFER_SIZE];
    char *commands[32];  /* Max 32 commands */
    int cmd_count = 0;
    int i;

    if (device == NULL || cfg == NULL || result == NULL) {
        return ERROR_INVALID_ARG;
    }

    result->status = HEALTH_STATUS_UNKNOWN;
    memset(result->message, 0, sizeof(result->message));

    /* Temporarily open serial port */
    serial_init(&port);
    if (serial_open(&port, device, cfg) != SUCCESS) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Cannot open serial port for modem check");
        return SUCCESS;
    }

    printf("\n=== Modem Command Execution ===\n");

    /* Step 1: Send AT command and check response */
    printf("Sending: AT\n");
    ssize_t n = send_at_command_and_wait(&port, "AT", response,
                                         sizeof(response), 2);

    if (n > 0) {
        printf("Response: ");
        /* Print response (handle non-printable chars) */
        for (ssize_t i = 0; i < n; i++) {
            if (response[i] >= 0x20 && response[i] <= 0x7E) {
                putchar(response[i]);
            } else if (response[i] == '\r' || response[i] == '\n') {
                /* Skip CR/LF for now, will print newline at end */
            }
        }
        printf("\n");

        result->status = HEALTH_STATUS_OK;
        snprintf(result->message, sizeof(result->message),
                "Modem responded to AT command");
    } else if (n == 0) {
        printf("Response: (timeout - no response)\n");
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "No response from modem (timeout 2s) - modem may be offline");
    } else {
        printf("Response: (read error)\n");
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "Read error from modem");
    }

    /* Step 2: Process MODEM_COMMAND if configured */
    if (cfg->modem_command[0] != '\0') {
        printf("\n--- Executing MODEM_COMMAND ---\n");
        printf("Raw MODEM_COMMAND: %s\n\n", cfg->modem_command);

        /* Parse commands (make a copy because strtok modifies string) */
        SAFE_STRNCPY(modem_cmd_copy, cfg->modem_command,
                     sizeof(modem_cmd_copy));
        cmd_count = parse_modem_commands(modem_cmd_copy, commands, 32);

        /* Send each command */
        for (i = 0; i < cmd_count; i++) {
            printf("Command %d/%d: %s\n", i + 1, cmd_count, commands[i]);

            memset(response, 0, sizeof(response));
            n = send_at_command_and_wait(&port, commands[i], response,
                                         sizeof(response), 2);

            if (n > 0) {
                printf("Response: ");
                for (ssize_t j = 0; j < n; j++) {
                    if (response[j] >= 0x20 && response[j] <= 0x7E) {
                        putchar(response[j]);
                    } else if (response[j] == '\r' || response[j] == '\n') {
                        /* Skip */
                    }
                }
                printf("\n");
            } else if (n == 0) {
                printf("Response: (timeout)\n");
            } else {
                printf("Response: (error)\n");
            }

            printf("\n");
        }

        printf("Total commands sent: %d\n", cmd_count);
    } else {
        printf("\nMODEM_COMMAND: (not configured)\n");
    }

    printf("================================\n\n");

    serial_close(&port);
    return SUCCESS;
}
```

---

### [6] healthcheck.c 수정 - 헬퍼 함수 추가 위치

**파일**: `src/healthcheck.c`

**위치**: `healthcheck_modem_device()` 함수 **앞**에 삽입 (라인 113-116 사이)

헬퍼 함수:
1. `send_at_command_and_wait()` - AT 명령 전송 및 응답 수신
2. `parse_modem_commands()` - 세미콜론으로 분리된 명령 파싱

---

### [7] 동작 흐름도

```
main()
  │
  ├─ config_load() → MODEM_COMMAND 읽기
  │
  ├─ healthcheck_run()
  │     │
  │     ├─ healthcheck_serial_port()
  │     │    └─ 결과: OK/WARNING/ERROR
  │     │
  │     ├─ healthcheck_modem_device()  ← 여기 수정!
  │     │    │
  │     │    ├─ Serial port 임시 오픈
  │     │    │
  │     │    ├─ [Step 1] AT 명령 전송
  │     │    │    ├─ 전송: "AT\r\n"
  │     │    │    ├─ 응답 대기 (2초)
  │     │    │    └─ 출력: "Response: OK" 또는 "(timeout)"
  │     │    │
  │     │    ├─ [Step 2] MODEM_COMMAND 파싱
  │     │    │    ├─ 입력: "AT&C1 B0 X3; ATS0=1H0"
  │     │    │    └─ 파싱: ["AT&C1 B0 X3", "ATS0=1H0"]
  │     │    │
  │     │    ├─ [Step 3] 각 명령 순차 전송
  │     │    │    ├─ 명령 1: "AT&C1 B0 X3\r\n"
  │     │    │    │    ├─ 전송
  │     │    │    │    ├─ 응답 대기 (2초)
  │     │    │    │    └─ 출력: "Response: ..."
  │     │    │    │
  │     │    │    └─ 명령 2: "ATS0=1H0\r\n"
  │     │    │         ├─ 전송
  │     │    │         ├─ 응답 대기 (2초)
  │     │    │         └─ 출력: "Response: ..."
  │     │    │
  │     │    └─ Serial port 닫기
  │     │
  │     └─ healthcheck_telnet_server()
  │
  └─ bridge_start()
       └─ 메인 루프...
```

---

### [8] 출력 예시

#### Case 1: Modem이 정상 응답하고 MODEM_COMMAND가 설정된 경우

```
=== Health Check ===

Serial Port:
  Status: OK
  Device exists and accessible: /dev/ttyUSB0

Modem Device:

=== Modem Command Execution ===
Sending: AT
Response: OK

--- Executing MODEM_COMMAND ---
Raw MODEM_COMMAND: AT&C1 B0 X3; ATS0=1H0

Command 1/2: AT&C1 B0 X3
Response: OK

Command 2/2: ATS0=1H0
Response: OK

Total commands sent: 2
================================

  Status: OK
  Modem responded to AT command

Telnet Server:
  Status: OK
  Connected: 127.0.0.1:8882

====================
```

#### Case 2: Modem이 응답하지 않지만 MODEM_COMMAND는 전송

```
=== Modem Command Execution ===
Sending: AT
Response: (timeout - no response)

--- Executing MODEM_COMMAND ---
Raw MODEM_COMMAND: AT&C1 B0 X3; ATS0=1H0

Command 1/2: AT&C1 B0 X3
Response: (timeout)

Command 2/2: ATS0=1H0
Response: (timeout)

Total commands sent: 2
================================

  Status: WARNING
  No response from modem (timeout 2s) - modem may be offline
```

#### Case 3: MODEM_COMMAND가 설정되지 않은 경우

```
=== Modem Command Execution ===
Sending: AT
Response: OK

MODEM_COMMAND: (not configured)
================================

  Status: OK
  Modem responded to AT command
```

---

## 테스트 시나리오

### Test 1: 정상 동작 테스트
- **설정**: `MODEM_COMMAND="AT&C1; ATE1; ATZ"`
- **예상**: 3개 명령 모두 전송, 각각의 응답 출력

### Test 2: 빈 MODEM_COMMAND
- **설정**: `MODEM_COMMAND=""`
- **예상**: AT 명령만 전송, "MODEM_COMMAND: (not configured)" 출력

### Test 3: MODEM_COMMAND 없음
- **설정**: 설정 파일에 `MODEM_COMMAND` 항목 없음
- **예상**: 기본값 빈 문자열, Test 2와 동일

### Test 4: 세미콜론 주변 공백
- **설정**: `MODEM_COMMAND="AT&C1  ;  ATE1  ;  ATZ"`
- **예상**: 공백 제거 후 정상 파싱

### Test 5: Modem 미연결 상태
- **설정**: Serial port 있지만 modem 미응답
- **예상**: AT timeout, MODEM_COMMAND도 timeout, 하지만 명령은 전송됨

---

## 파일 수정 요약

| 파일 | 수정 내용 |
|------|----------|
| `include/config.h` | `config_t`에 `modem_command` 필드 추가 |
| `src/config.c` | `config_init()`에 기본값 초기화, `parse_config_line()`에 파싱 로직 추가 |
| `src/healthcheck.c` | 헬퍼 함수 2개 추가, `healthcheck_modem_device()` 전체 재작성 |

---

## 구현 순서

1. **config.h** 수정 (구조체 필드 추가)
2. **config.c** 수정 (초기화 + 파싱)
3. **healthcheck.c** 수정 (헬퍼 함수 + modem device check)
4. **컴파일 테스트** (`make clean && make`)
5. **기능 테스트** (다양한 설정으로 실행)

---

## 주의사항

1. **Thread Safety**: 현재 코드는 single-thread이므로 문제없음
2. **Buffer Overflow**: `SAFE_STRNCPY` 사용으로 방지
3. **NULL 체크**: 모든 함수에서 인자 검증
4. **Error Handling**: Serial I/O 오류는 warning으로 처리, 서버는 계속 실행
5. **Memory Leak**: `strtok_r` 사용, 동적 할당 없음
6. **Timeout**: 각 명령당 2초 timeout (충분함)

---

## 향후 확장 가능성

1. **MODEM_RESPONSE_TIMEOUT** 설정 추가
2. **MODEM_COMMAND_RETRY** 실패 시 재시도 횟수
3. **로그 파일에도 기록** (현재는 stdout만)
4. **Health check 재실행 명령** (SIGUSR1 등)
