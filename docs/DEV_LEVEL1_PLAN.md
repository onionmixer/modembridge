# DEV_LEVEL1_PLAN - Level 1 개발 계획

## 개요
modem_sample의 검증된 패턴을 ModemBridge Level 1에 단계적으로 적용하는 상세 구현 계획입니다.

## Phase 1: UUCP Port Locking 및 Serial 초기화

### 목표
안정적인 시리얼 포트 접근 및 초기화 시퀀스 구현

### 구현 항목
1. **UUCP Lock 파일 구현** (serial.c)
   ```c
   // Lock 파일 생성: /var/lock/LCK..ttyUSB0
   int serial_lock_port(const char *device) {
       char lockfile[PATH_MAX];
       snprintf(lockfile, sizeof(lockfile), "/var/lock/LCK..%s",
                basename(device));

       int fd = open(lockfile, O_CREAT | O_EXCL | O_WRONLY, 0644);
       if (fd < 0) return ERROR_PORT_LOCKED;

       char pid_str[32];
       snprintf(pid_str, sizeof(pid_str), "%10d\n", getpid());
       write(fd, pid_str, strlen(pid_str));
       close(fd);
       return SUCCESS;
   }
   ```

2. **시리얼 포트 초기화 순서**
   - Lock 획득 → Port Open → Configure → DTR/RTS 설정
   - OPOST/ONLCR 비활성화 (raw mode)
   - CLOCAL 설정 (모뎀 제어 신호 무시)

### 테스트 방법
- 동시 접근 시도로 lock 동작 확인
- stty로 포트 설정 검증

## Phase 2: Line Buffering 구현

### 목표
완전한 라인 단위 읽기로 메시지 단편화 방지

### 구현 항목
1. **serial_read_line() 함수** (serial.c)
   ```c
   typedef struct {
       char buffer[LINE_BUFFER_SIZE];
       size_t pos;
       bool has_partial;
   } line_buffer_t;

   int serial_read_line(serial_t *serial, char *line,
                       size_t max_len, int timeout_ms) {
       // 내부 버퍼에서 완전한 라인 추출
       // \r\n 또는 \n으로 끝나는 라인 반환
       // 타임아웃 시 ERROR_TIMEOUT
   }
   ```

2. **하드웨어 메시지 버퍼 통합** (modem.c)
   ```c
   // 기존 hw_msg_buffer 활용
   modem->hw_msg_buffer[modem->hw_msg_len++] = ch;
   if (ch == '\n' || ch == '\r') {
       // 완전한 하드웨어 메시지 처리
       process_hardware_message(modem);
   }
   ```

### 테스트 방법
- "CONNECT 57600" 같은 긴 메시지 전송
- 단편화 없이 완전한 메시지 수신 확인

## Phase 3: 모뎀 초기화 및 RING 감지

### 목표
안정적인 모뎀 초기화 및 RING 신호 처리

### 구현 항목
1. **초기화 시퀀스** (bridge.c)
   ```c
   int initialize_modem_sequence(bridge_t *ctx) {
       // 1. DTR 설정
       serial_set_dtr(ctx->serial, 1);
       usleep(500000);

       // 2. 버퍼 드레이닝 (RING 감지 포함)
       drain_serial_buffer_with_ring_detection(ctx);

       // 3. ATZ 소프트 리셋
       modem_send_command(ctx->modem, "ATZ");
       wait_for_ok(ctx, 2000);

       // 4. MODEM_INIT_COMMAND 실행
       execute_init_commands(ctx);

       // 5. Auto-answer 설정
       setup_autoanswer_mode(ctx);
   }
   ```

2. **RING 감지 통합** (bridge.c:800-828)
   ```c
   // Draining 중 RING 체크
   if (strstr(buffer, "RING") != NULL) {
       ctx->ring_count++;
       if (ctx->config->modem_autoanswer_mode == 1 &&
           ctx->ring_count >= 2) {
           // SOFTWARE 모드: 2 RING 후 ATA
           modem_send_command(ctx->modem, "ATA");
       }
   }
   ```

### 테스트 방법
- RING 신호 시뮬레이션
- SOFTWARE/HARDWARE 모드별 동작 확인

## Phase 4: 연결 수립 및 속도 감지

### 목표
안정적인 연결 수립 및 통신 속도 자동 감지

### 구현 항목
1. **CONNECT 응답 파싱**
   ```c
   int parse_connect_response(const char *response) {
       // "CONNECT 57600" → 57600 추출
       // "CONNECT" only → 기본 속도 사용
       int speed = 0;
       if (sscanf(response, "CONNECT %d", &speed) == 1) {
           return speed;
       }
       return DEFAULT_SPEED;
   }
   ```

2. **속도별 시리얼 포트 재설정**
   ```c
   int adjust_serial_speed(serial_t *serial, int speed) {
       speed_t baud = speed_to_baud(speed);
       return serial_set_baudrate(serial, baud);
   }
   ```

### 테스트 방법
- 다양한 속도로 연결 시도
- 속도 변경 후 데이터 전송 확인

## Phase 5: Carrier 감지 및 안정적 전송

### 목표
DCD 신호 모니터링 및 연결 상태 관리

### 구현 항목
1. **DCD 모니터링**
   ```c
   bool check_carrier(serial_t *serial) {
       int status;
       ioctl(serial->fd, TIOCMGET, &status);
       return (status & TIOCM_CAR) != 0;
   }
   ```

2. **연결 상태 관리**
   ```c
   // 주기적 carrier 체크
   if (!check_carrier(ctx->serial)) {
       // NO CARRIER 처리
       modem_hangup(ctx->modem);
       bridge_disconnect(ctx);
   }
   ```

### 테스트 방법
- 연결 중 강제 종료 시뮬레이션
- NO CARRIER 감지 및 복구 확인

## Health Check 통합

### MODEM_COMMAND 기능 (구현 완료)
```c
// config.h
char modem_command[LINE_BUFFER_SIZE];  // Health check commands

// healthcheck.c
if (cfg->modem_command[0] != '\0') {
    // ';'로 구분된 명령어 실행
    execute_modem_commands(cfg->modem_command);
}
```

### 시리얼 초기화 표시 (Phase 6 - 미구현)
```c
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
    return serial_configure(serial, config);
}
```

## 구현 우선순위

### 완료된 항목 ✅
1. Serial port lock/unlock 순서 수정
2. Select() with timeout 구현
3. RING 감지 및 처리
4. MODEM_COMMAND health check 통합
5. 버퍼 드레이닝 개선

### 진행 중 🔄
1. Line buffering 완전 통합
2. Carrier 감지 구현

### 미구현 ❌
1. 시리얼 초기화 명시적 표시
2. 속도 자동 감지 및 조정
3. UUCP lock 파일 완전 구현

## 위험 요소 및 대응

| 위험 | 영향도 | 대응 방안 |
|------|--------|----------|
| 시리얼 포트 블로킹 | 높음 | select() 타임아웃 적용 완료 |
| CONNECT 메시지 분할 | 높음 | Line buffering 구현 중 |
| RING 신호 누락 | 중간 | Draining 중 RING 체크 완료 |
| Lock 파일 충돌 | 낮음 | PID 검증 로직 추가 예정 |

## 테스트 계획

### 단위 테스트
- [ ] serial_read_line() 라인 버퍼링
- [ ] UUCP lock 파일 생성/제거
- [ ] CONNECT 응답 파싱
- [ ] DCD 신호 감지

### 통합 테스트
- [ ] 전체 초기화 시퀀스
- [ ] RING → ATA → CONNECT 흐름
- [ ] 30분 연속 운영 안정성
- [ ] 멀티바이트 문자 전송

### 성능 테스트
- [ ] 다양한 baudrate 전환
- [ ] 대용량 데이터 전송
- [ ] CPU/메모리 사용률

## 일정
- Phase 1-2: 핵심 기능 (1주)
- Phase 3-4: 모뎀 동작 (1주)
- Phase 5-6: 안정성 (3일)
- 테스트 및 문서화: (3일)