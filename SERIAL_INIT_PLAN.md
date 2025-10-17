# Serial Port 초기화 개선 계획

## 목표

Health check 과정에서 serial port 체크와 modem 체크 사이에 명시적인 serial port 초기화 단계를 추가하여, 설정 내용을 사용자에게 명확히 보여줍니다.

## 현재 문제점

1. `healthcheck_serial_port()`: 파일 존재/권한만 체크
2. `healthcheck_modem_device()`: 내부적으로 `serial_open()` → AT 체크 → `serial_close()`
3. 사용자는 어떤 설정으로 serial port가 초기화되는지 알 수 없음

## 개선 방안

### 새로운 흐름

```
1. healthcheck_serial_port()
   - Device 파일 존재 확인
   - Character device 확인
   - 읽기/쓰기 권한 확인
   - 임시 open/close 테스트

2. [NEW] healthcheck_serial_init()
   - Serial port를 config 설정으로 초기화
   - termios 설정 적용
   - 적용된 설정 출력:
     * Baudrate: 9600
     * Data bits: 8
     * Parity: NONE
     * Stop bits: 1
     * Flow control: NONE

3. healthcheck_modem_device()
   - 이미 초기화된 serial port로 AT 명령 체크
   - MODEM_COMMAND 실행 (healthcheck_print_report에서)
```

## 구현 계획

### [14-1] health_report_t에 serial_init 결과 추가

**파일**: `include/healthcheck.h`

```c
typedef struct {
    health_check_result_t serial_port;
    health_check_result_t serial_init;      // ← 추가
    health_check_result_t modem_device;
    health_check_result_t telnet_server;
} health_report_t;
```

### [14-2] healthcheck_serial_init() 함수 추가

**파일**: `src/healthcheck.c`

```c
/**
 * Initialize serial port with config settings
 * Opens the port, applies termios settings, and reports configuration
 */
int healthcheck_serial_init(const char *device, const config_t *cfg,
                            health_check_result_t *result)
{
    serial_port_t port;

    if (device == NULL || cfg == NULL || result == NULL) {
        return ERROR_INVALID_ARG;
    }

    result->status = HEALTH_STATUS_UNKNOWN;
    memset(result->message, 0, sizeof(result->message));

    /* Initialize and open serial port */
    serial_init(&port);
    if (serial_open(&port, device, cfg) != SUCCESS) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Failed to initialize serial port");
        return SUCCESS;
    }

    /* Success - port is now initialized with config settings */
    result->status = HEALTH_STATUS_OK;
    snprintf(result->message, sizeof(result->message),
            "Serial port initialized: %d baud, %d%c%d, flow=%s",
            cfg->baudrate_value,
            cfg->data_bits,
            cfg->parity == PARITY_NONE ? 'N' :
            cfg->parity == PARITY_EVEN ? 'E' : 'O',
            cfg->stop_bits,
            config_flow_to_str(cfg->flow_control));

    serial_close(&port);
    return SUCCESS;
}
```

### [14-3] healthcheck_run() 수정

**파일**: `src/healthcheck.c`

```c
int healthcheck_run(const config_t *cfg, health_report_t *report)
{
    if (cfg == NULL || report == NULL) {
        return ERROR_INVALID_ARG;
    }

    memset(report, 0, sizeof(health_report_t));

    /* Check serial port */
    healthcheck_serial_port(cfg->serial_port, &report->serial_port);

    /* Initialize serial port (only if serial port is accessible) */
    if (report->serial_port.status == HEALTH_STATUS_OK) {
        healthcheck_serial_init(cfg->serial_port, cfg, &report->serial_init);
    } else {
        report->serial_init.status = HEALTH_STATUS_ERROR;
        SAFE_STRNCPY(report->serial_init.message,
                    "Cannot initialize (serial port not available)",
                    sizeof(report->serial_init.message));
    }

    /* Check modem device (only if serial init succeeded) */
    if (report->serial_init.status == HEALTH_STATUS_OK) {
        healthcheck_modem_device(cfg->serial_port, cfg, &report->modem_device);
    } else {
        report->modem_device.status = HEALTH_STATUS_ERROR;
        SAFE_STRNCPY(report->modem_device.message,
                    "Cannot check modem (serial not initialized)",
                    sizeof(report->modem_device.message));
    }

    /* Check telnet server */
    healthcheck_telnet_server(cfg->telnet_host, cfg->telnet_port,
                             &report->telnet_server);

    return SUCCESS;
}
```

### [14-4] healthcheck_print_report() 수정

**파일**: `src/healthcheck.c`

```c
void healthcheck_print_report(const health_report_t *report, const config_t *cfg)
{
    if (report == NULL) {
        return;
    }

    printf("=== Health Check ===\n");
    printf("\n");

    printf("Serial Port:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->serial_port.status));
    printf("  %s\n", report->serial_port.message);
    printf("\n");

    // ← 추가
    printf("Serial Initialization:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->serial_init.status));
    printf("  %s\n", report->serial_init.message);
    printf("\n");

    printf("Modem Device:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->modem_device.status));
    printf("  %s\n", report->modem_device.message);

    /* Execute MODEM_COMMAND if modem is accessible */
    if (cfg != NULL &&
        report->serial_init.status == HEALTH_STATUS_OK &&  // ← 변경
        (report->modem_device.status == HEALTH_STATUS_OK ||
         report->modem_device.status == HEALTH_STATUS_WARNING)) {
        // ... MODEM_COMMAND 실행 로직
    }

    // ... 나머지 출력
}
```

## 예상 출력

```
=== Health Check ===

Serial Port:
  Status: OK
  Device exists and accessible: /dev/ttyUSB0

Serial Initialization:
  Status: OK
  Serial port initialized: 9600 baud, 8N1, flow=NONE

Modem Device:
  Status: OK
  Modem responded to AT command

  === Modem Command Execution ===
  Sending: AT
  Response (4 bytes): [HEX: 41 54 0D 0A ] [ASCII: AT]

  --- Executing MODEM_COMMAND ---
  Raw MODEM_COMMAND: AT&C1 B0 X3; ATS0=1H0

  Command 1/2: AT&C1 B0 X3
  Response (13 bytes): [HEX: 41 54 26 43 31 20 42 30 20 58 33 0D 0A ] [ASCII: AT&C1 B0 X3]

  Command 2/2: ATS0=1H0
  Response (7 bytes): [HEX: 41 54 53 30 3D 31 48 30 ] [ASCII: ATS0=1H0]

  Total commands sent: 2
  ================================

Telnet Server:
  Status: OK
  Connected: 127.0.0.1:8882
====================
```

## 구현 순서

1. `health_report_t`에 `serial_init` 필드 추가
2. `healthcheck_serial_init()` 함수 구현
3. `healthcheck_run()` 수정 (초기화 단계 추가)
4. `healthcheck_print_report()` 수정 (Serial Initialization 섹션 추가)
5. 컴파일 및 테스트

## 이점

1. **명확한 흐름**: Serial 체크 → 초기화 → Modem 체크 순서가 명확
2. **설정 가시성**: 사용자가 어떤 설정으로 초기화되는지 확인 가능
3. **디버깅 용이**: 각 단계별 성공/실패 상태 확인 가능
4. **기존 코드 유지**: termios 기반 초기화는 그대로 유지

## 주의사항

- `serial_open()`과 `serial_close()`는 각 단계에서 독립적으로 수행
- `healthcheck_serial_init()`에서 초기화만 하고 닫음
- `healthcheck_print_report()`의 MODEM_COMMAND 실행 시 재오픈
- 실제 bridge 동작 시에는 별도로 `serial_open()` 호출
