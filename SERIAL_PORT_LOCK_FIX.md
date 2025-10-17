# Serial Port Lock/Unlock 순서 수정

## 문제 발견

사용자가 보고한 로그:
```
[DEBUG] Attempting to lock serial port: /dev/ttyUSB0
[DEBUG] Serial port locked successfully
[DEBUG] Attempting to open serial port: /dev/ttyUSB0
[DEBUG] serial_open() returned: -3 (SUCCESS=0)
[DEBUG] Serial port open FAILED, entering retry mode
```

## 근본 원인

**이중 locking** 문제:

1. `bridge.c:582` - `serial_lock_port()` 호출 → **성공** ✓
2. `serial.c:44` (수정 전) - `serial_open()` 내부에서 다시 `serial_lock_port()` 호출 → **실패** ✗
   - 이미 같은 프로세스가 lock을 가지고 있어서 실패
3. `serial_open()`이 `ERROR_IO (-3)` 반환

## modem_sample과의 비교

### modem_sample의 올바른 순서 (serial_port.c:44-115)

```c
int open_serial_port(const char *device, int baudrate)
{
    // 1. Lock port FIRST (caller responsibility)
    if (lock_port(device) != SUCCESS) {
        return ERROR_PORT;
    }

    // 2. THEN open serial device
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);

    // 3. Configure terminal settings
    // 4. Set blocking mode

    return fd;
}

void close_serial_port(int fd)
{
    // Close port
    close(fd);

    // Unlock at the end (caller responsibility)
    unlock_port();
}
```

### modembridge의 잘못된 구조 (수정 전)

```c
// bridge.c:
int bridge_start(bridge_ctx_t *ctx)
{
    serial_lock_port();     // 첫 번째 lock
    serial_open();          // 내부에서 두 번째 lock 시도 → 실패!
    ...
}

// serial.c:
int serial_open(...)
{
    serial_lock_port();     // 이중 lock!
    ...
    serial_unlock_port();   // 에러 시 unlock
}

int serial_close(...)
{
    ...
    serial_unlock_port();   // 닫을 때 unlock
}
```

**문제점:**
- Lock이 두 번 시도됨
- Lock/unlock 책임이 serial.c와 bridge.c에 분산됨
- modem_sample 패턴과 불일치

## 수정 내용

### 1. serial.c - Lock/Unlock 코드 제거

**serial_open() 수정** (serial.c:41-51):
```c
// 수정 전:
MB_LOG_INFO("Opening serial port: %s", device);

/* Step 1: Lock port using UUCP-style locking (modem_sample pattern) */
if (serial_lock_port(device) != SUCCESS) {
    MB_LOG_ERROR("Failed to lock port %s", device);
    return ERROR_IO;
}

/* Step 2: Open serial device... */
port->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
if (port->fd < 0) {
    serial_unlock_port();  /* Unlock on failure */
    return ERROR_IO;
}

// 수정 후:
MB_LOG_INFO("Opening serial port: %s", device);

/* NOTE: Port locking should be done by caller BEFORE calling serial_open() */
/* This matches modem_sample pattern where lock_port() is called first */

/* Step 1: Open serial device with O_NOCTTY | O_NONBLOCK (modem_sample pattern) */
port->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
if (port->fd < 0) {
    MB_LOG_ERROR("Failed to open %s: %s", device, strerror(errno));
    return ERROR_IO;
}
```

**에러 핸들링에서 unlock 제거** (serial.c:57-89):
```c
// 수정 전:
if (tcgetattr(port->fd, &port->oldtio) < 0) {
    close(port->fd);
    port->fd = -1;
    serial_unlock_port();  // ← 제거됨
    return ERROR_IO;
}

if (ret != SUCCESS) {
    close(port->fd);
    port->fd = -1;
    serial_unlock_port();  // ← 제거됨
    return ret;
}
```

**serial_close() 수정** (serial.c:114-141):
```c
// 수정 전:
if (!port->is_open || port->fd < 0) {
    serial_unlock_port();  // ← 제거됨
    return SUCCESS;
}

close(port->fd);
port->fd = -1;
port->is_open = false;

serial_unlock_port();  // ← 제거됨
MB_LOG_INFO("Serial port closed and unlocked");

// 수정 후:
if (!port->is_open || port->fd < 0) {
    return SUCCESS;
}

close(port->fd);
port->fd = -1;
port->is_open = false;

/* NOTE: Port unlocking should be done by caller AFTER serial_close() */
/* This matches modem_sample pattern where unlock_port() is called in cleanup */

MB_LOG_INFO("Serial port closed");
```

### 2. bridge.c - Lock/Unlock 책임 유지

**현재 bridge.c는 이미 올바른 순서를 따르고 있음:**

```c
// bridge_start() - lines 579-593, 939-946:
serial_lock_port(device);           // Lock FIRST
serial_open(port, device, config);  // Then open
// ... work ...

// bridge_stop() - lines 939-946:
serial_close(port);     // Close FIRST
serial_unlock_port();   // Then unlock
```

## 올바른 순서 (modem_sample 패턴)

### 초기화 시퀀스:
```
1. serial_lock_port(device)         ← bridge.c
2. serial_open(port, device, cfg)   ← serial.c (내부에서 lock 안 함)
3. modem_init(&modem, &serial)
```

### 종료 시퀀스:
```
1. modem_hangup(&modem)
2. serial_close(&serial)            ← serial.c (내부에서 unlock 안 함)
3. serial_unlock_port()             ← bridge.c
```

## 테스트 결과

### 수정 전:
```
[DEBUG] Attempting to lock serial port: /dev/ttyUSB0
[DEBUG] Serial port locked successfully
[DEBUG] Attempting to open serial port: /dev/ttyUSB0
[DEBUG] serial_open() returned: -3 (SUCCESS=0)  ← ERROR_IO
[DEBUG] Serial port open FAILED, entering retry mode
```

### 수정 후 (예상):
```
[DEBUG] Attempting to lock serial port: /dev/ttyUSB0
[DEBUG] Serial port locked successfully
[DEBUG] Attempting to open serial port: /dev/ttyUSB0
[INFO] Opening serial port: /dev/ttyUSB0
[DEBUG] Serial port converted to blocking mode
[DEBUG] serial_open() returned: 0 (SUCCESS=0)   ← SUCCESS!
[DEBUG] Serial port opened successfully
```

## 핵심 원칙 (modem_sample 패턴)

1. **Lock/Unlock은 호출자(caller) 책임**
   - Lock은 serial_open() **전**에 호출
   - Unlock은 serial_close() **후**에 호출

2. **serial.c는 lock을 관리하지 않음**
   - serial_open()은 이미 lock된 포트를 가정
   - serial_close()는 unlock하지 않음
   - 오직 serial_lock_port(), serial_unlock_port() 함수만 제공

3. **에러 핸들링 단순화**
   - serial_open() 실패 시 호출자가 unlock 처리
   - 중복 unlock 방지

## 관련 파일

- `src/serial.c` - Lock/Unlock 코드 제거
- `src/bridge.c` - Lock/Unlock 관리 (수정 없음, 이미 올바름)
- `include/serial.h` - 함수 선언 (변경 없음)

## 참고

- modem_sample/serial_port.c:44-115 (open_serial_port)
- modem_sample/serial_port.c:121-139 (close_serial_port)
- modem_sample/serial_port.c:513-578 (lock_port, unlock_port)
