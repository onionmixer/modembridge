# ModemBridge Level1 Development Plan - Based on modem_sample Analysis

## Executive Summary

This development plan outlines the systematic integration of proven techniques from the successful `modem_sample` implementation into the current ModemBridge level1 code. The modem_sample code has been thoroughly tested and proven to work reliably with real modem hardware, making its approaches the priority for our implementation.

## Core Principle

**The modem_sample's serial port and modem handling methods take absolute priority over current implementation approaches.**

## Implementation Phases

### Phase 1: Foundation Layer - Serial Port Core Improvements

#### 1.1 UUCP-Style Port Locking (Priority: CRITICAL)

**Current Issue:** No port locking, risk of multiple access conflicts

**Implementation from modem_sample:**

```c
// Add to serial.c
#include <sys/file.h>

static int lock_port(const char *device) {
    char lockfile[256];
    char pidstr[32];
    int lock_fd;

    // Extract device name (e.g., ttyUSB0 from /dev/ttyUSB0)
    const char *dev_name = strrchr(device, '/');
    if (dev_name) dev_name++;
    else dev_name = device;

    // Create lock file path: /var/lock/LCK..ttyUSB0
    snprintf(lockfile, sizeof(lockfile), "/var/lock/LCK..%s", dev_name);

    // Try to create lock file
    lock_fd = open(lockfile, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (lock_fd < 0) {
        if (errno == EEXIST) {
            // Check if lock is stale
            lock_fd = open(lockfile, O_RDONLY);
            if (lock_fd >= 0) {
                char old_pid[32];
                int n = read(lock_fd, old_pid, sizeof(old_pid)-1);
                close(lock_fd);
                if (n > 0) {
                    old_pid[n] = '\0';
                    int pid = atoi(old_pid);
                    // Check if process still exists
                    if (kill(pid, 0) < 0 && errno == ESRCH) {
                        // Stale lock, remove it
                        unlink(lockfile);
                        // Retry creation
                        lock_fd = open(lockfile, O_WRONLY | O_CREAT | O_EXCL, 0644);
                    }
                }
            }
        }
        if (lock_fd < 0) {
            MB_LOG_ERROR("Cannot lock port %s: %s", device, strerror(errno));
            return -1;
        }
    }

    // Write our PID to lock file
    snprintf(pidstr, sizeof(pidstr), "%10d\n", getpid());
    write(lock_fd, pidstr, strlen(pidstr));
    close(lock_fd);

    return 0;
}

static void unlock_port(const char *device) {
    char lockfile[256];
    const char *dev_name = strrchr(device, '/');
    if (dev_name) dev_name++;
    else dev_name = device;

    snprintf(lockfile, sizeof(lockfile), "/var/lock/LCK..%s", dev_name);
    unlink(lockfile);
}
```

**Integration Point:** Call `lock_port()` at the beginning of `serial_open()` and `unlock_port()` in `serial_close()`

#### 1.2 Serial Port Initialization Sequence (Priority: CRITICAL)

**Current Issue:** Improper flag sequence, no saved state for restoration

**Implementation from modem_sample:**

```c
// Replace current serial_open() implementation
int serial_open(const char *device, int baudrate) {
    int fd;
    struct termios current_tios, saved_tios;
    speed_t speed;

    // Step 1: Lock port FIRST
    if (lock_port(device) < 0) {
        return -1;
    }

    // Step 2: Open with specific flags
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        MB_LOG_ERROR("Cannot open %s: %s", device, strerror(errno));
        unlock_port(device);
        return -1;
    }

    // Step 3: Save original settings for restoration
    if (tcgetattr(fd, &saved_tios) < 0) {
        MB_LOG_ERROR("tcgetattr failed: %s", strerror(errno));
        close(fd);
        unlock_port(device);
        return -1;
    }

    // Store saved settings (add to serial context structure)
    // serial_ctx->saved_tios = saved_tios;

    // Step 4: Configure raw mode (modem_sample pattern)
    current_tios = saved_tios;

    // Input flags: NO processing
    current_tios.c_iflag = 0;

    // Output flags: CR->CRLF conversion
    current_tios.c_oflag = OPOST | ONLCR;

    // Control flags
    current_tios.c_cflag &= ~(CSTOPB | PARENB | PARODD);
    current_tios.c_cflag |= CS8 | CREAD | HUPCL | CLOCAL;

    // Local flags: raw mode
    current_tios.c_lflag = 0;

    // Control characters
    current_tios.c_cc[VMIN] = 1;   // At least 1 byte
    current_tios.c_cc[VTIME] = 0;  // No timeout

    // Set baudrate
    speed = baud_to_speed(baudrate);
    cfsetispeed(&current_tios, speed);
    cfsetospeed(&current_tios, speed);

    // Step 5: Apply with TCSADRAIN (wait for output)
    if (tcsetattr(fd, TCSADRAIN, &current_tios) < 0) {
        MB_LOG_ERROR("tcsetattr failed: %s", strerror(errno));
        close(fd);
        unlock_port(device);
        return -1;
    }

    // Step 6: Convert to blocking mode AFTER setup
    int flags = fcntl(fd, F_GETFL, 0);
    flags &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) {
        MB_LOG_ERROR("fcntl failed: %s", strerror(errno));
        close(fd);
        unlock_port(device);
        return -1;
    }

    // Step 7: Flush buffers
    tcflush(fd, TCIOFLUSH);

    MB_LOG_INFO("Serial port %s opened at %d baud", device, baudrate);
    return fd;
}
```

### Phase 2: Advanced Serial I/O Functions

#### 2.1 Line Reading with Internal Buffering (Priority: HIGH)

**Current Issue:** Basic read without buffering, loses data on fragmentation

**Implementation from modem_sample:**

```c
// Add to serial.c
#define LINE_BUFFER_SIZE 256
#define READ_CHUNK_SIZE 128

typedef struct {
    char buffer[512];      // Internal buffer
    size_t pos;           // Current read position
    size_t len;           // Data available
} LineReadContext;

static LineReadContext line_ctx = {0};  // Static context

int serial_read_line(int fd, char *buffer, int size, int timeout_sec) {
    char chunk[READ_CHUNK_SIZE];
    time_t start_time = time(NULL);
    int remaining_timeout;
    int rc;
    size_t i;

    while (1) {
        // Search for line terminator in existing buffer
        for (i = line_ctx.pos; i < line_ctx.len; i++) {
            if (line_ctx.buffer[i] == '\n' || line_ctx.buffer[i] == '\r') {
                // Found complete line
                size_t copy_len = i - line_ctx.pos;
                if (copy_len >= size) copy_len = size - 1;

                memcpy(buffer, &line_ctx.buffer[line_ctx.pos], copy_len);
                buffer[copy_len] = '\0';

                // Skip all CR/LF characters
                line_ctx.pos = i + 1;
                while (line_ctx.pos < line_ctx.len &&
                       (line_ctx.buffer[line_ctx.pos] == '\r' ||
                        line_ctx.buffer[line_ctx.pos] == '\n')) {
                    line_ctx.pos++;
                }

                // Reset if consumed all
                if (line_ctx.pos >= line_ctx.len) {
                    line_ctx.pos = 0;
                    line_ctx.len = 0;
                }

                MB_LOG_DEBUG("Read line: %s", buffer);
                return copy_len;
            }
        }

        // Check timeout
        remaining_timeout = timeout_sec - (time(NULL) - start_time);
        if (remaining_timeout <= 0) {
            MB_LOG_WARNING("Line read timeout");
            return ERROR_TIMEOUT;
        }

        // Compact buffer if needed
        if (line_ctx.pos > 0) {
            memmove(line_ctx.buffer, &line_ctx.buffer[line_ctx.pos],
                    line_ctx.len - line_ctx.pos);
            line_ctx.len -= line_ctx.pos;
            line_ctx.pos = 0;
        }

        // Check for buffer overflow
        if (line_ctx.len >= sizeof(line_ctx.buffer) - READ_CHUNK_SIZE) {
            MB_LOG_ERROR("Line buffer overflow");
            line_ctx.pos = 0;
            line_ctx.len = 0;
            return ERROR_GENERAL;
        }

        // Read more data
        rc = serial_read_with_timeout(fd, chunk, READ_CHUNK_SIZE, 1);

        if (rc > 0) {
            // Append to internal buffer
            memcpy(&line_ctx.buffer[line_ctx.len], chunk, rc);
            line_ctx.len += rc;
        }
        else if (rc < 0 && rc != ERROR_TIMEOUT) {
            return rc;  // Propagate error
        }
    }
}
```

#### 2.2 Carrier Detection System (Priority: HIGH)

**Current Issue:** No carrier monitoring, can't detect disconnections

**Implementation from modem_sample:**

```c
// Add to serial.c

// Enable carrier detection (call after connection established)
int serial_enable_carrier_detect(int fd) {
    struct termios tios;

    if (tcgetattr(fd, &tios) < 0) {
        MB_LOG_ERROR("tcgetattr failed: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    // Clear CLOCAL to enable DCD monitoring
    tios.c_cflag &= ~CLOCAL;

    // Enable hardware flow control
    tios.c_cflag |= CRTSCTS;

    if (tcsetattr(fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("tcsetattr failed: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    MB_LOG_INFO("Carrier detection enabled");
    return SUCCESS;
}

// Check carrier status
int serial_check_carrier(int fd) {
    int status;

    if (ioctl(fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("ioctl TIOCMGET failed: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    // TIOCM_CAR is the DCD signal
    return (status & TIOCM_CAR) ? 1 : 0;
}

// Verify carrier before transmission
int serial_verify_carrier_before_send(int fd) {
    int carrier = serial_check_carrier(fd);

    if (carrier <= 0) {
        MB_LOG_ERROR("Carrier lost - cannot transmit");
        return ERROR_HANGUP;
    }

    return SUCCESS;
}
```

#### 2.3 Robust Write with Retry Logic (Priority: CRITICAL)

**Current Issue:** No retry on partial writes or temporary failures

**Implementation from modem_sample:**

```c
// Add to serial.c
#define MAX_WRITE_RETRY 3
#define RETRY_DELAY_US 100000  // 100ms

int serial_write_robust(int fd, const char *data, int len) {
    int sent = 0;
    int retry = 0;
    int rc;

    // Pre-transmission carrier check
    if (serial_verify_carrier_before_send(fd) != SUCCESS) {
        return ERROR_HANGUP;
    }

    // Main transmission loop with retry
    while (sent < len && retry < MAX_WRITE_RETRY) {
        rc = write(fd, data + sent, len - sent);

        if (rc < 0) {
            // Check for connection loss
            if (errno == EPIPE || errno == ECONNRESET) {
                MB_LOG_ERROR("Connection hangup during write");
                return ERROR_HANGUP;
            }

            // Retry on temporary errors
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                MB_LOG_WARNING("Write would block, retry %d/%d",
                              retry + 1, MAX_WRITE_RETRY);
                usleep(RETRY_DELAY_US);
                retry++;
                continue;
            }

            // Fatal errors
            MB_LOG_ERROR("Write error: %s", strerror(errno));
            return ERROR_GENERAL;
        }

        // Successful write
        sent += rc;
        retry = 0;  // Reset retry counter

        // Log partial write
        if (rc > 0 && rc < (len - sent)) {
            MB_LOG_INFO("Partial write: %d of %d bytes", sent, len);
        }
    }

    if (sent < len) {
        MB_LOG_ERROR("Failed after %d retries: sent %d of %d",
                    MAX_WRITE_RETRY, sent, len);
        return ERROR_GENERAL;
    }

    // Wait for transmission to complete
    tcdrain(fd);

    MB_LOG_DEBUG("Wrote %d bytes successfully", sent);
    return sent;
}
```

### Phase 3: Modem Control Enhancements

#### 3.1 DTR Drop Hardware Hangup (Priority: HIGH)

**Current Issue:** No hardware hangup implementation

**Implementation from modem_sample:**

```c
// Add to modem.c
int modem_dtr_drop_hangup(int fd) {
    struct termios tios;
    speed_t saved_ispeed, saved_ospeed;

    MB_LOG_INFO("Performing DTR drop hardware hangup...");

    // Get current settings
    if (tcgetattr(fd, &tios) < 0) {
        MB_LOG_ERROR("tcgetattr failed: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    // Save current speeds
    saved_ispeed = cfgetispeed(&tios);
    saved_ospeed = cfgetospeed(&tios);

    // Set speed to 0 to drop DTR
    cfsetispeed(&tios, B0);
    cfsetospeed(&tios, B0);

    if (tcsetattr(fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("Failed to drop DTR: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    // Wait for modem to recognize hangup
    sleep(1);

    // Restore original speeds
    cfsetispeed(&tios, saved_ispeed);
    cfsetospeed(&tios, saved_ospeed);

    if (tcsetattr(fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("Failed to restore DTR: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    MB_LOG_INFO("DTR drop hangup completed");
    return SUCCESS;
}
```

#### 3.2 Enhanced AT Command Processing (Priority: HIGH)

**Current Issue:** Basic AT command handling without proper response parsing

**Implementation from modem_sample pattern:**

```c
// Enhance modem.c
int modem_send_at_command(int fd, const char *command,
                         char *response, int resp_size, int timeout) {
    char cmd_buf[256];
    char line_buf[256];
    int rc;
    time_t start = time(NULL);
    int remaining;

    // Clear input buffer
    tcflush(fd, TCIFLUSH);

    // Format and send command
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r", command);
    rc = serial_write_robust(fd, cmd_buf, strlen(cmd_buf));

    if (rc < 0) {
        return rc;
    }

    // Small delay after command
    usleep(100000);  // 100ms

    // Read response lines
    response[0] = '\0';

    while (1) {
        remaining = timeout - (time(NULL) - start);
        if (remaining <= 0) {
            MB_LOG_WARNING("AT command timeout");
            return ERROR_TIMEOUT;
        }

        rc = serial_read_line(fd, line_buf, sizeof(line_buf), remaining);

        if (rc > 0) {
            MB_LOG_DEBUG("AT response: %s", line_buf);

            // Append to response buffer if provided
            if (response && resp_size > 0) {
                strncat(response, line_buf, resp_size - strlen(response) - 1);
                strncat(response, "\n", resp_size - strlen(response) - 1);
            }

            // Check for CONNECT (with speed extraction)
            if (strstr(line_buf, "CONNECT") != NULL) {
                // Extract speed if present
                int speed = 0;
                char *speed_str = strstr(line_buf, "CONNECT ");
                if (speed_str) {
                    speed = atoi(speed_str + 8);
                    MB_LOG_INFO("Connected at %d baud", speed);
                }
                return SUCCESS;
            }

            // Check error responses
            if (strstr(line_buf, "NO CARRIER")) return ERROR_NO_CARRIER;
            if (strstr(line_buf, "BUSY")) return ERROR_BUSY;
            if (strstr(line_buf, "NO DIALTONE")) return ERROR_NO_DIALTONE;
            if (strstr(line_buf, "NO ANSWER")) return ERROR_NO_ANSWER;
            if (strstr(line_buf, "ERROR")) return ERROR_MODEM;

            // Check for OK
            if (strstr(line_buf, "OK")) {
                return SUCCESS;
            }
        }
        else if (rc < 0 && rc != ERROR_TIMEOUT) {
            return rc;
        }
    }
}
```

#### 3.3 Dynamic Speed Adjustment (Priority: MEDIUM)

**Current Issue:** Fixed baudrate, no adjustment based on connection

**Implementation:**

```c
// Add to modem.c
int modem_adjust_speed(int fd, int connected_speed) {
    struct termios tios;
    speed_t speed;

    MB_LOG_INFO("Adjusting serial speed to %d baud", connected_speed);

    // Map connected speed to speed_t
    speed = baud_to_speed(connected_speed);
    if (speed == B0) {
        MB_LOG_ERROR("Invalid speed: %d", connected_speed);
        return ERROR_GENERAL;
    }

    // Get current settings
    if (tcgetattr(fd, &tios) < 0) {
        return ERROR_GENERAL;
    }

    // Set new speed
    cfsetispeed(&tios, speed);
    cfsetospeed(&tios, speed);

    // Apply settings
    if (tcsetattr(fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("Failed to set speed: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    // Small delay for hardware adjustment
    usleep(50000);  // 50ms

    MB_LOG_INFO("Speed adjusted successfully");
    return SUCCESS;
}
```

### Phase 4: Error Handling and Logging

#### 4.1 Comprehensive Error Codes (Priority: HIGH)

**Add to common.h:**

```c
// Enhanced error codes from modem_sample
#define SUCCESS           0
#define ERROR_GENERAL    -1
#define ERROR_TIMEOUT    -2
#define ERROR_MODEM      -3
#define ERROR_SERIAL     -4
#define ERROR_HANGUP     -5    // Carrier lost/connection dropped
#define ERROR_NO_CARRIER -6
#define ERROR_BUSY       -7
#define ERROR_NO_DIALTONE -8
#define ERROR_NO_ANSWER  -9
#define ERROR_PARTIAL    -10   // Partial write/read

// Helper function for error strings
const char* error_to_string(int error_code);
```

#### 4.2 Transmission Logging (Priority: MEDIUM)

**Add debug logging capability:**

```c
// Add to common.c
void log_transmission(const char *tag, const char *data, int len) {
#ifdef DEBUG
    char hex_buf[1024];
    int i, pos = 0;

    for (i = 0; i < len && pos < sizeof(hex_buf) - 4; i++) {
        pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos,
                       "%02X ", (unsigned char)data[i]);
    }

    MB_LOG_DEBUG("[%s] TX %d bytes: %s", tag, len, hex_buf);

    // Also log as ASCII if printable
    int printable = 1;
    for (i = 0; i < len; i++) {
        if (!isprint(data[i]) && data[i] != '\r' && data[i] != '\n') {
            printable = 0;
            break;
        }
    }

    if (printable) {
        char ascii_buf[256];
        int copy_len = (len < sizeof(ascii_buf) - 1) ? len : sizeof(ascii_buf) - 1;
        memcpy(ascii_buf, data, copy_len);
        ascii_buf[copy_len] = '\0';
        MB_LOG_DEBUG("[%s] ASCII: %s", tag, ascii_buf);
    }
#endif
}
```

### Phase 5: Integration into Main Bridge Loop

#### 5.1 Updated Bridge Run Loop

**Modify bridge.c:**

```c
int bridge_run(BridgeContext *ctx) {
    fd_set read_fds;
    struct timeval tv;
    int max_fd;
    int rc;

    MB_LOG_INFO("Bridge loop starting");

    // Enable carrier detection after connection
    if (ctx->state == STATE_CONNECTED) {
        serial_enable_carrier_detect(ctx->serial_fd);
    }

    while (!ctx->shutdown_flag) {
        FD_ZERO(&read_fds);
        FD_SET(ctx->serial_fd, &read_fds);
        max_fd = ctx->serial_fd;

        if (ctx->telnet_fd >= 0) {
            FD_SET(ctx->telnet_fd, &read_fds);
            if (ctx->telnet_fd > max_fd) {
                max_fd = ctx->telnet_fd;
            }
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        rc = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (rc < 0) {
            if (errno == EINTR) continue;
            MB_LOG_ERROR("select failed: %s", strerror(errno));
            break;
        }

        // Check carrier status periodically
        if (ctx->state == STATE_CONNECTED) {
            if (serial_check_carrier(ctx->serial_fd) <= 0) {
                MB_LOG_WARNING("Carrier lost - initiating disconnect");
                ctx->state = STATE_DISCONNECTING;
                bridge_disconnect(ctx);
                continue;
            }
        }

        // Handle serial data
        if (FD_ISSET(ctx->serial_fd, &read_fds)) {
            rc = bridge_handle_serial_data(ctx);
            if (rc == ERROR_HANGUP) {
                MB_LOG_INFO("Hangup detected from serial");
                ctx->state = STATE_DISCONNECTING;
                bridge_disconnect(ctx);
            }
        }

        // Handle telnet data
        if (ctx->telnet_fd >= 0 && FD_ISSET(ctx->telnet_fd, &read_fds)) {
            rc = bridge_handle_telnet_data(ctx);
            if (rc < 0) {
                MB_LOG_INFO("Telnet connection closed");
                ctx->state = STATE_DISCONNECTING;
                bridge_disconnect(ctx);
            }
        }
    }

    return SUCCESS;
}
```

## Implementation Schedule

### Week 1: Foundation
- Day 1-2: Implement port locking and serial initialization refactoring
- Day 3-4: Add line reading with internal buffering
- Day 5: Testing and debugging foundation layer

### Week 2: Core Functions
- Day 1-2: Implement carrier detection system
- Day 3-4: Add robust write with retry logic
- Day 5: Implement DTR drop hangup

### Week 3: Modem Control
- Day 1-2: Upgrade AT command processing
- Day 3: Add dynamic speed adjustment
- Day 4-5: Integration testing with real modem

### Week 4: Polish and Testing
- Day 1-2: Add comprehensive error codes and logging
- Day 3-4: Full system integration testing
- Day 5: Documentation and final adjustments

## Testing Strategy

### Unit Tests
1. Port locking with concurrent access attempts
2. Line reading with fragmented data
3. Carrier detection simulation
4. Retry logic verification

### Integration Tests
1. Full connection sequence with real modem
2. Data transmission reliability test
3. Hangup detection and recovery
4. Speed negotiation testing

### Hardware Testing
```bash
# Virtual serial port testing
socat -d -d pty,raw,echo=0 pty,raw,echo=0

# Real modem testing checklist
- [ ] AT command responses
- [ ] Connection establishment
- [ ] Data transmission
- [ ] Escape sequence (+++)
- [ ] Hardware hangup (DTR drop)
- [ ] Carrier loss detection
```

## Success Criteria

1. **Reliability**: Zero data loss during transmission
2. **Compatibility**: Works with standard Hayes-compatible modems
3. **Error Handling**: Graceful recovery from all error conditions
4. **Performance**: Maintains connection stability for extended periods
5. **Logging**: Comprehensive debug information for troubleshooting

## Files to Modify

Priority order based on dependency:

1. `include/common.h` - Add error codes and structures
2. `src/serial.c` - Implement all serial improvements
3. `src/modem.c` - Add enhanced AT command handling
4. `src/bridge.c` - Integrate carrier detection and error handling
5. `src/main.c` - Update initialization sequence
6. `src/config.c` - Add new configuration options if needed

## Configuration Changes

Add to modembridge.conf:
```ini
# Serial port retry settings
SERIAL_MAX_RETRY=3
SERIAL_RETRY_DELAY_MS=100

# Transmission settings
TX_CHUNK_SIZE=256
TX_CHUNK_DELAY_MS=10

# Carrier detection
ENABLE_CARRIER_DETECT=true
CARRIER_CHECK_INTERVAL_SEC=5

# Speed adjustment
AUTO_ADJUST_SPEED=true
```

## Risk Mitigation

1. **Backward Compatibility**: Maintain existing API where possible
2. **Gradual Integration**: Test each phase independently
3. **Fallback Mode**: Keep original functions available via config flag
4. **Extensive Logging**: Add debug logs at every critical point
5. **Hardware Variations**: Test with multiple modem models

## Conclusion

This plan prioritizes the proven techniques from modem_sample, particularly:

1. **UUCP-style port locking** - Prevents conflicts
2. **Internal buffering for line reading** - Handles fragmentation
3. **Carrier detection and verification** - Detects disconnections
4. **Robust write with retry** - Ensures data delivery
5. **DTR drop hangup** - Hardware-level connection control

By following this implementation plan, the ModemBridge level1 will achieve the same reliability and robustness demonstrated by the modem_sample code.