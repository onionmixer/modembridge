# DEV_LEVEL2_PLAN - Level 2 개발 계획

## 개요
Telnet 클라이언트 기능을 단계적으로 구현하여 RFC 표준을 준수하는 안정적인 텔넷 통신 계층을 구축합니다.

## Phase 1: 기본 Telnet 연결 및 IAC 처리

### 목표
TCP 연결 수립 및 기본 IAC 명령 파싱

### 구현 항목

#### 1.1 Telnet 연결 관리
```c
// telnet.c
typedef struct {
    int socket_fd;
    char host[256];
    int port;
    telnet_state_t state;
    circular_buffer_t rx_buffer;
    circular_buffer_t tx_buffer;
    iac_parser_t parser;
} telnet_t;

int telnet_connect(telnet_t *telnet, const char *host, int port) {
    struct sockaddr_in server_addr;

    telnet->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (telnet->socket_fd < 0) {
        return ERROR_SOCKET;
    }

    // Non-blocking mode
    int flags = fcntl(telnet->socket_fd, F_GETFL, 0);
    fcntl(telnet->socket_fd, F_SETFL, flags | O_NONBLOCK);

    // Connect
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &server_addr.sin_addr);

    return connect(telnet->socket_fd,
                   (struct sockaddr*)&server_addr,
                   sizeof(server_addr));
}
```

#### 1.2 IAC 파서 상태 기계
```c
// telnet.c
typedef enum {
    TELNET_STATE_DATA,
    TELNET_STATE_IAC,
    TELNET_STATE_WILL,
    TELNET_STATE_WONT,
    TELNET_STATE_DO,
    TELNET_STATE_DONT,
    TELNET_STATE_SB,
    TELNET_STATE_SB_IAC
} telnet_parse_state_t;

int telnet_process_input(telnet_t *telnet,
                         const uint8_t *data,
                         size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        switch (telnet->parser.state) {
            case TELNET_STATE_DATA:
                if (byte == IAC) {
                    telnet->parser.state = TELNET_STATE_IAC;
                } else {
                    // Normal data
                    buffer_write(&telnet->rx_buffer, byte);
                }
                break;

            case TELNET_STATE_IAC:
                if (byte == IAC) {
                    // Escaped 0xFF
                    buffer_write(&telnet->rx_buffer, 0xFF);
                    telnet->parser.state = TELNET_STATE_DATA;
                } else {
                    // Process IAC command
                    process_iac_command(telnet, byte);
                }
                break;
            // ... other states
        }
    }
    return SUCCESS;
}
```

### 테스트 방법
- telnet localhost 9091 연결 테스트
- IAC 명령 수신 시 로깅 확인
- 0xFF 데이터 이스케이핑 검증

## Phase 2: 옵션 협상 구현

### 목표
DO/DONT/WILL/WONT 협상 메커니즘 구현

### 구현 항목

#### 2.1 옵션 상태 관리
```c
// telnet.h
typedef struct {
    bool local_enabled;   // We WILL
    bool remote_enabled;  // They DO
} telnet_option_t;

typedef struct {
    telnet_option_t options[256];  // All possible options
    uint8_t supported_options[32]; // Bitmap of supported
} telnet_options_t;
```

#### 2.2 협상 응답 로직
```c
// telnet.c
int telnet_handle_do(telnet_t *telnet, uint8_t option) {
    MB_LOG_DEBUG("Received DO %d", option);

    if (is_supported_option(telnet, option)) {
        if (!telnet->options.options[option].local_enabled) {
            // Accept - send WILL
            telnet_send_command(telnet, TELNET_WILL, option);
            telnet->options.options[option].local_enabled = true;

            // Option-specific initialization
            handle_option_enabled(telnet, option, true);
        }
    } else {
        // Reject - send WONT
        telnet_send_command(telnet, TELNET_WONT, option);
    }
    return SUCCESS;
}

int telnet_handle_will(telnet_t *telnet, uint8_t option) {
    MB_LOG_DEBUG("Received WILL %d", option);

    if (is_desired_option(telnet, option)) {
        if (!telnet->options.options[option].remote_enabled) {
            // Accept - send DO
            telnet_send_command(telnet, TELNET_DO, option);
            telnet->options.options[option].remote_enabled = true;
        }
    } else {
        // Reject - send DONT
        telnet_send_command(telnet, TELNET_DONT, option);
    }
    return SUCCESS;
}
```

### 테스트 방법
- 옵션 협상 시퀀스 로깅
- Wireshark로 협상 패킷 분석
- 순환 협상 방지 확인

## Phase 3: 핵심 옵션 구현

### 목표
SGA, ECHO, TERMINAL-TYPE 옵션 구현

### 구현 항목

#### 3.1 SGA (Suppress Go Ahead) 처리
```c
// telnet.c
#define TELOPT_SGA 3

int handle_sga_option(telnet_t *telnet, bool enabled) {
    if (enabled) {
        // Suppress GA - character mode
        telnet->suppress_ga = true;
        MB_LOG_INFO("SGA enabled - character mode");
    } else {
        // Send GA after each line
        telnet->suppress_ga = false;
        MB_LOG_INFO("SGA disabled - line mode");
    }
    return SUCCESS;
}
```

#### 3.2 ECHO 옵션 제어
```c
// telnet.c
#define TELOPT_ECHO 1

int handle_echo_option(telnet_t *telnet, bool local, bool enabled) {
    if (local) {
        // We echo
        telnet->local_echo = enabled;
    } else {
        // They echo
        telnet->remote_echo = enabled;
    }

    // Prevent double echo
    if (telnet->local_echo && telnet->remote_echo) {
        MB_LOG_WARNING("Both sides echoing - disabling local");
        telnet->local_echo = false;
    }
    return SUCCESS;
}
```

#### 3.3 TERMINAL-TYPE 서브협상
```c
// telnet.c
#define TELOPT_TTYPE 24
#define TELQUAL_IS 0
#define TELQUAL_SEND 1

int handle_ttype_subnegotiation(telnet_t *telnet,
                                const uint8_t *data,
                                size_t len) {
    if (len > 0 && data[0] == TELQUAL_SEND) {
        // Server requests our terminal type
        const char *term_type = "VT100";

        uint8_t response[64];
        int pos = 0;
        response[pos++] = IAC;
        response[pos++] = SB;
        response[pos++] = TELOPT_TTYPE;
        response[pos++] = TELQUAL_IS;

        strcpy((char*)&response[pos], term_type);
        pos += strlen(term_type);

        response[pos++] = IAC;
        response[pos++] = SE;

        telnet_send_raw(telnet, response, pos);
        MB_LOG_INFO("Sent terminal type: %s", term_type);
    }
    return SUCCESS;
}
```

### 테스트 방법
- 문자 모드 서버 (9092) 연결
- SGA+ECHO 조합 테스트
- Terminal type 교환 확인

## Phase 4: 모드별 동작 구현

### 목표
라인 모드와 문자 모드 전환 및 처리

### 구현 항목

#### 4.1 라인 모드 처리
```c
// telnet.c
typedef struct {
    char line_buffer[1024];
    size_t line_pos;
    bool line_mode;
} telnet_line_t;

int telnet_process_line_mode(telnet_t *telnet,
                             const char *data,
                             size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];

        if (ch == '\r' || ch == '\n') {
            // Send complete line
            telnet_send_line(telnet,
                            telnet->line.line_buffer,
                            telnet->line.line_pos);
            telnet->line.line_pos = 0;
        } else if (ch == 0x7F || ch == 0x08) {
            // Backspace
            if (telnet->line.line_pos > 0) {
                telnet->line.line_pos--;
            }
        } else {
            // Add to line buffer
            if (telnet->line.line_pos < sizeof(telnet->line.line_buffer) - 1) {
                telnet->line.line_buffer[telnet->line.line_pos++] = ch;
            }
        }
    }
    return SUCCESS;
}
```

#### 4.2 문자 모드 처리
```c
// telnet.c
int telnet_process_char_mode(telnet_t *telnet,
                             const char *data,
                             size_t len) {
    // Send each character immediately
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = data[i];

        // Check for IAC escape
        if (ch == 0xFF) {
            uint8_t iac_seq[2] = {0xFF, 0xFF};
            telnet_send_raw(telnet, iac_seq, 2);
        } else {
            telnet_send_raw(telnet, &ch, 1);
        }
    }
    return SUCCESS;
}
```

### 테스트 방법
- 라인 모드 서버 (9091) 테스트
- 문자 모드 서버 (9092) 테스트
- 모드 전환 시나리오

## Phase 5: 테스트 모듈 구현

### 목표
자동화된 테스트 시나리오 실행

### 구현 항목

#### 5.1 테스트 전송 모듈
```c
// telnet_test.c (임시 모듈)
typedef struct {
    const char *text;
    int delay_ms;
} test_message_t;

static test_message_t test_messages[] = {
    {"abcd", 3000},
    {"한글", 3000},
    {"こんにちは。", 3000},
    {NULL, 0}
};

void* telnet_test_thread(void *arg) {
    telnet_t *telnet = (telnet_t*)arg;

    MB_LOG_INFO("Starting telnet test sequence");

    for (int i = 0; test_messages[i].text != NULL; i++) {
        MB_LOG_INFO("Sending test message: %s", test_messages[i].text);

        telnet_send_data(telnet,
                        test_messages[i].text,
                        strlen(test_messages[i].text));

        usleep(test_messages[i].delay_ms * 1000);
    }

    MB_LOG_INFO("Test sequence completed");
    return NULL;
}
```

#### 5.2 타임스탬프 수신 검증
```c
// telnet_test.c
int verify_timestamp_reception(telnet_t *telnet, int duration_sec) {
    time_t start_time = time(NULL);
    int timestamp_count = 0;

    MB_LOG_INFO("Waiting for timestamps for %d seconds", duration_sec);

    while (time(NULL) - start_time < duration_sec) {
        char buffer[256];
        int len = telnet_receive(telnet, buffer, sizeof(buffer));

        if (len > 0) {
            buffer[len] = '\0';
            if (strstr(buffer, "timestamp") != NULL ||
                strstr(buffer, "time:") != NULL) {
                timestamp_count++;
                MB_LOG_INFO("Timestamp received: %s", buffer);
            }
        }

        usleep(100000); // 100ms
    }

    MB_LOG_INFO("Total timestamps received: %d", timestamp_count);
    return timestamp_count > 0 ? SUCCESS : ERROR_TIMEOUT;
}
```

### 테스트 방법
- 각 서버 포트별 자동 테스트
- 결과를 level2_telnet_test_result.txt 저장
- 로그 분석 및 검증

## Phase 6: LINEMODE 옵션 (선택적)

### 목표
RFC 1184 LINEMODE 구현

### 구현 항목
```c
// telnet.c
#define TELOPT_LINEMODE 34
#define LM_MODE 1
#define LM_FORWARDMASK 2
#define LM_SLC 3

typedef struct {
    uint8_t mode;
    uint8_t forward_mask[32];
    uint8_t slc[32][3];
} linemode_state_t;

int handle_linemode_subnegotiation(telnet_t *telnet,
                                   const uint8_t *data,
                                   size_t len) {
    // Parse LINEMODE suboptions
    // MODE, FORWARDMASK, SLC handling
    // ...
}
```

## 구현 일정

| Phase | 기간 | 상태 | 비고 |
|-------|------|------|------|
| Phase 1: 기본 IAC | 3일 | 🔄 | 핵심 기능 |
| Phase 2: 옵션 협상 | 2일 | ⏸️ | DO/WILL 메커니즘 |
| Phase 3: 핵심 옵션 | 3일 | ⏸️ | SGA, ECHO, TTYPE |
| Phase 4: 모드 처리 | 2일 | ⏸️ | 라인/문자 모드 |
| Phase 5: 테스트 | 2일 | ⏸️ | 자동화 테스트 |
| Phase 6: LINEMODE | 선택 | ❌ | 추가 기능 |

## 위험 관리

| 위험 요소 | 영향도 | 대응 방안 |
|-----------|--------|----------|
| IAC 파싱 오류 | 높음 | 상태 기계 철저한 테스트 |
| 옵션 순환 협상 | 중간 | 상태 추적 및 중복 방지 |
| 멀티바이트 문자 | 중간 | UTF-8 경계 보존 |
| 버퍼 오버플로우 | 높음 | 크기 제한 및 검증 |

## 성공 기준

- [ ] 3가지 서버 모두 연결 성공
- [ ] IAC 명령 올바른 처리
- [ ] 멀티바이트 문자 무결성
- [ ] 30분 연속 운영
- [ ] 자동 테스트 통과