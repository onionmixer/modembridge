# ModemBridge API Reference

## Table of Contents

1. [Overview](#overview)
2. [Module Architecture](#module-architecture)
3. [Core Data Structures](#core-data-structures)
4. [Serial Port API (serial.c)](#serial-port-api-serialc)
5. [Modem Emulation API (modem.c)](#modem-emulation-api-modemc)
6. [Telnet Protocol API (telnet.c)](#telnet-protocol-api-telnetc)
7. [Bridge Engine API (bridge.c)](#bridge-engine-api-bridgec)
8. [Configuration API (config.c)](#configuration-api-configc)
9. [Common Utilities (common.c)](#common-utilities-commonc)
10. [Logging System](#logging-system)
11. [Error Handling](#error-handling)
12. [Extension Guide](#extension-guide)

---

## Overview

ModemBridge is implemented in C11/GNU11 with zero external dependencies beyond glibc and POSIX APIs. The codebase follows a layered architecture with clear separation of concerns.

**Design Principles:**
- **Layered architecture**: Each layer has well-defined responsibilities
- **Non-blocking I/O**: All operations use non-blocking file descriptors
- **Event-driven**: `select()` multiplexes serial and telnet I/O
- **State machines**: Modem and telnet both maintain state machines
- **Buffer safety**: All operations bounds-checked, UTF-8 aware

**Code Style:**
- K&R-style bracing
- 4-space indentation
- snake_case for functions and variables
- UPPER_CASE for macros and constants
- Comprehensive error checking on all system calls

---

## Module Architecture

```
┌─────────────────────────────────────┐
│         main.c (Daemon)             │  Signal handling, daemonization
├─────────────────────────────────────┤
│       bridge.c (Core Engine)        │  I/O multiplexing, data routing
├──────────────┬──────────────────────┤
│   modem.c    │     telnet.c         │  Protocol handlers
│ (AT Commands)│  (RFC 854, IAC)      │
├──────────────┴──────────────────────┤
│   serial.c (termios/POSIX)          │  Hardware abstraction
├─────────────────────────────────────┤
│   config.c, common.c (Utils)        │  Support infrastructure
└─────────────────────────────────────┘
```

**Data Flow:**

**Serial → Telnet:**
1. `serial_read()` - Read from USB serial port
2. `modem_process_input()` - Handle AT commands or detect escape
3. `ansi_filter_modem_to_telnet()` - Remove ANSI cursor codes
4. `telnet_prepare_output()` - Escape IAC bytes (0xFF → 0xFF 0xFF)
5. `telnet_send()` - Send to telnet server

**Telnet → Serial:**
1. `telnet_recv()` - Read from telnet server
2. `telnet_process_input()` - Parse IAC sequences
3. `ansi_passthrough_telnet_to_modem()` - Pass ANSI unchanged
4. `serial_write()` - Send to modem client

---

## Core Data Structures

### bridge_t

**File**: `include/bridge.h`

Main bridge context structure that holds all state.

```c
typedef struct {
    /* Serial port state */
    int serial_fd;
    char serial_port[256];
    int baudrate;

    /* Modem emulation */
    modem_t modem;

    /* Telnet connection */
    telnet_t telnet;

    /* Connection state */
    connection_state_t state;

    /* Buffers */
    char serial_buffer[BUFFER_SIZE];
    char telnet_buffer[BUFFER_SIZE];

    /* Configuration */
    config_t *config;

    /* Data logging */
    FILE *data_log_file;
} bridge_t;
```

**Usage:**
```c
bridge_t bridge;
memset(&bridge, 0, sizeof(bridge));
bridge.config = config;
bridge_init(&bridge);
```

---

### modem_t

**File**: `include/modem.h`

Modem emulation state and settings.

```c
typedef struct {
    /* Command processing */
    char cmd_buffer[CMD_BUFFER_SIZE];
    int cmd_pos;

    /* State machine */
    modem_state_t state;

    /* Escape sequence detection */
    int escape_count;
    time_t escape_timer;

    /* Settings */
    modem_settings_t settings;

    /* Serial file descriptor (reference) */
    int serial_fd;
} modem_t;
```

**Modem States:**
```c
typedef enum {
    MODEM_STATE_COMMAND,        /* Processing AT commands */
    MODEM_STATE_ONLINE,         /* Transparent data mode */
    MODEM_STATE_CONNECTING,     /* Establishing connection */
    MODEM_STATE_DISCONNECTED    /* Idle, no connection */
} modem_state_t;
```

---

### modem_settings_t

**File**: `include/modem.h`

Configuration for modem behavior (AT command settings).

```c
typedef struct {
    bool echo;                  /* ATE: Command echo */
    bool verbose;               /* ATV: Verbose responses */
    bool quiet;                 /* ATQ: Quiet mode */
    int s_registers[256];       /* ATS: S-registers */

    /* Extended settings (Phase 16) */
    int dcd_mode;               /* AT&C: DCD control */
    int dtr_mode;               /* AT&D: DTR handling */
    int bell_mode;              /* ATB: Bell/CCITT */
    int result_mode;            /* ATX: Extended result codes */
    int speaker_volume;         /* ATL: Speaker volume */
    int speaker_control;        /* ATM: Speaker control */
    int error_correction;       /* AT\N: Error correction */
    int dsr_mode;               /* AT&S: DSR override */

    /* Configuration storage */
    bool profile_saved[2];      /* AT&W: Profile saved status */
} modem_settings_t;
```

---

### telnet_t

**File**: `include/telnet.h`

Telnet connection state and protocol handler.

```c
typedef struct {
    int sockfd;                 /* Socket file descriptor */
    char host[256];             /* Server hostname */
    int port;                   /* Server port */

    /* Telnet protocol state */
    telnet_state_t state;
    unsigned char iac_cmd;
    unsigned char iac_opt;

    /* Option negotiation */
    bool binary_mode;
    bool echo_mode;
    bool suppress_go_ahead;

    /* Connection state */
    bool connected;
} telnet_t;
```

---

### config_t

**File**: `include/config.h`

Configuration loaded from modembridge.conf.

```c
typedef struct {
    /* Serial port configuration */
    char serial_port[256];
    int baudrate;
    parity_t parity;
    int data_bits;
    int stop_bits;
    flow_control_t flow_control;

    /* Modem initialization */
    char modem_init_command[1024];      /* Phase 13 */
    char modem_autoanswer_command[1024]; /* Phase 15 */

    /* Telnet configuration */
    char telnet_host[256];
    int telnet_port;

    /* Data logging */
    bool data_log_enabled;
    char data_log_file[256];
} config_t;
```

---

## Serial Port API (serial.c)

### serial_init()

Initialize serial port with configuration.

**Prototype:**
```c
int serial_init(const char *device, int baudrate, parity_t parity,
                int data_bits, int stop_bits, flow_control_t flow);
```

**Parameters:**
- `device`: Serial port path (e.g., `/dev/ttyUSB0`)
- `baudrate`: Speed in bps (300-230400)
- `parity`: `PARITY_NONE`, `PARITY_ODD`, `PARITY_EVEN`
- `data_bits`: 7 or 8
- `stop_bits`: 1 or 2
- `flow`: `FLOW_NONE`, `FLOW_HARDWARE`, `FLOW_SOFTWARE`

**Returns:**
- File descriptor (>= 0) on success
- -1 on error

**Example:**
```c
int fd = serial_init("/dev/ttyUSB0", 115200, PARITY_NONE, 8, 1, FLOW_NONE);
if (fd < 0) {
    MB_LOG_ERROR("Failed to initialize serial port");
    return -1;
}
```

**Implementation Details:**
- Opens port with `O_RDWR | O_NOCTTY | O_NONBLOCK`
- Saves original termios for restoration
- Configures raw mode (no canonical processing)
- Sets VMIN=0, VTIME=0 for non-blocking reads
- Applies flow control settings

---

### serial_close()

Close serial port and restore original settings.

**Prototype:**
```c
void serial_close(int fd);
```

**Parameters:**
- `fd`: File descriptor from `serial_init()`

**Example:**
```c
serial_close(fd);
```

**Implementation Details:**
- Restores original termios settings
- Flushes buffers with `tcflush()`
- Closes file descriptor

---

### serial_read()

Read data from serial port (non-blocking).

**Prototype:**
```c
ssize_t serial_read(int fd, void *buf, size_t count);
```

**Parameters:**
- `fd`: Serial port file descriptor
- `buf`: Buffer to receive data
- `count`: Maximum bytes to read

**Returns:**
- Number of bytes read (>= 0)
- -1 on error (check errno)
- 0 if no data available (EAGAIN)

**Example:**
```c
char buffer[1024];
ssize_t n = serial_read(fd, buffer, sizeof(buffer));
if (n > 0) {
    /* Process n bytes in buffer */
} else if (n < 0 && errno != EAGAIN) {
    MB_LOG_ERROR("Read error: %s", strerror(errno));
}
```

---

### serial_write()

Write data to serial port.

**Prototype:**
```c
ssize_t serial_write(int fd, const void *buf, size_t count);
```

**Parameters:**
- `fd`: Serial port file descriptor
- `buf`: Data to write
- `count`: Number of bytes to write

**Returns:**
- Number of bytes written (>= 0)
- -1 on error

**Example:**
```c
const char *msg = "OK\r\n";
ssize_t n = serial_write(fd, msg, strlen(msg));
if (n < 0) {
    MB_LOG_ERROR("Write error: %s", strerror(errno));
}
```

---

### serial_set_baudrate()

Change baudrate dynamically.

**Prototype:**
```c
int serial_set_baudrate(int fd, int baudrate);
```

**Parameters:**
- `fd`: Serial port file descriptor
- `baudrate`: New baudrate (300-230400)

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
if (serial_set_baudrate(fd, 57600) < 0) {
    MB_LOG_ERROR("Failed to set baudrate");
}
```

---

### serial_drain()

Wait for output to complete.

**Prototype:**
```c
int serial_drain(int fd);
```

**Parameters:**
- `fd`: Serial port file descriptor

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
serial_write(fd, data, len);
serial_drain(fd);  /* Wait for transmission */
```

---

## Modem Emulation API (modem.c)

### modem_init()

Initialize modem emulation.

**Prototype:**
```c
void modem_init(modem_t *modem, int serial_fd);
```

**Parameters:**
- `modem`: Modem structure to initialize
- `serial_fd`: Serial port file descriptor

**Example:**
```c
modem_t modem;
modem_init(&modem, serial_fd);
```

**Implementation Details:**
- Resets all state to defaults
- Initializes S-registers with standard values
- Sets state to `MODEM_STATE_COMMAND`
- Stores serial_fd reference for sending responses

---

### modem_reset()

Reset modem to default configuration.

**Prototype:**
```c
void modem_reset(modem_t *modem);
```

**Parameters:**
- `modem`: Modem structure to reset

**Example:**
```c
modem_reset(&modem);
```

**Implementation Details:**
- Restores factory default settings
- Resets all S-registers to defaults
- Sets state to `MODEM_STATE_COMMAND`
- Does NOT close connections (use `modem_hangup()`)

---

### modem_process_input()

Process data from serial port.

**Prototype:**
```c
int modem_process_input(modem_t *modem, const char *data, size_t len,
                        char *output, size_t *output_len);
```

**Parameters:**
- `modem`: Modem structure
- `data`: Input data from serial port
- `len`: Length of input data
- `output`: Buffer for pass-through data (when in online mode)
- `output_len`: Returns length of output data

**Returns:**
- 0 on success
- -1 on error

**Behavior:**
- **Command mode**: Accumulates characters, processes AT commands on CR
- **Online mode**: Detects escape sequence (+++), copies data to output

**Example:**
```c
char output[1024];
size_t output_len;

int ret = modem_process_input(&modem, input, input_len, output, &output_len);
if (ret == 0 && output_len > 0) {
    /* Send output to telnet */
    telnet_send(&telnet, output, output_len);
}
```

---

### modem_process_command()

Process a single AT command.

**Prototype:**
```c
int modem_process_command(modem_t *modem, const char *cmd);
```

**Parameters:**
- `modem`: Modem structure
- `cmd`: AT command string (without CR/LF)

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
modem_process_command(&modem, "ATE1V1");
```

**Implementation Details:**
- Parses command character by character
- Supports command chaining (e.g., `ATE1V1Q0`)
- Sends response via `modem_send_response()`
- Returns immediately on first error

---

### modem_send_response()

Send result code to serial port.

**Prototype:**
```c
int modem_send_response(modem_t *modem, const char *response);
```

**Parameters:**
- `modem`: Modem structure
- `response`: Result code string (e.g., `MODEM_RESP_OK`)

**Returns:**
- 0 on success
- -1 on error

**Response Codes:**
```c
#define MODEM_RESP_OK              "OK"
#define MODEM_RESP_ERROR           "ERROR"
#define MODEM_RESP_CONNECT         "CONNECT"
#define MODEM_RESP_NO_CARRIER      "NO CARRIER"
#define MODEM_RESP_RING            "RING"
#define MODEM_RESP_NO_DIALTONE     "NO DIALTONE"
#define MODEM_RESP_BUSY            "BUSY"
#define MODEM_RESP_NO_ANSWER       "NO ANSWER"
```

**Example:**
```c
modem_send_response(&modem, MODEM_RESP_OK);
modem_send_response_fmt(&modem, "CONNECT %d", baudrate);
```

**Implementation Details:**
- Respects quiet mode (ATQ1 = no responses)
- Converts to numeric if verbose mode off (ATV0)
- Filters responses based on ATX mode
- Adds CR/LF formatting

---

### modem_send_response_fmt()

Send formatted response to serial port.

**Prototype:**
```c
int modem_send_response_fmt(modem_t *modem, const char *fmt, ...);
```

**Parameters:**
- `modem`: Modem structure
- `fmt`: printf-style format string
- `...`: Format arguments

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
modem_send_response_fmt(&modem, "S%02d:%03d", reg, value);
modem_send_response_fmt(&modem, "CONNECT %d", baudrate);
```

---

### modem_set_state()

Change modem state.

**Prototype:**
```c
void modem_set_state(modem_t *modem, modem_state_t new_state);
```

**Parameters:**
- `modem`: Modem structure
- `new_state`: New state value

**States:**
- `MODEM_STATE_COMMAND` - Processing AT commands
- `MODEM_STATE_ONLINE` - Transparent data mode
- `MODEM_STATE_CONNECTING` - Establishing connection
- `MODEM_STATE_DISCONNECTED` - Idle

**Example:**
```c
modem_set_state(&modem, MODEM_STATE_ONLINE);
```

---

### modem_answer()

Answer incoming connection (ATA command).

**Prototype:**
```c
int modem_answer(modem_t *modem);
```

**Parameters:**
- `modem`: Modem structure

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
if (modem_answer(&modem) == 0) {
    /* Switch to online mode */
}
```

---

### modem_hangup()

Disconnect and return to command mode (ATH command).

**Prototype:**
```c
int modem_hangup(modem_t *modem);
```

**Parameters:**
- `modem`: Modem structure

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
modem_hangup(&modem);
modem_send_response(&modem, MODEM_RESP_NO_CARRIER);
```

---

### modem_show_configuration()

Display current configuration (AT&V command).

**Prototype:**
```c
int modem_show_configuration(modem_t *modem);
```

**Parameters:**
- `modem`: Modem structure

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
modem_show_configuration(&modem);
```

**Output Format:**
```
ACTIVE PROFILE:
E1 Q0 V1 X4
&C1 &D2 &S0
B0 L2 M1 \N3

S-REGISTERS:
S00:002 S01:000 S02:043 S03:013
...
OK
```

---

## Telnet Protocol API (telnet.c)

### telnet_init()

Initialize telnet connection structure.

**Prototype:**
```c
void telnet_init(telnet_t *telnet, const char *host, int port);
```

**Parameters:**
- `telnet`: Telnet structure to initialize
- `host`: Server hostname or IP address
- `port`: Server port number

**Example:**
```c
telnet_t telnet;
telnet_init(&telnet, "telehack.com", 23);
```

---

### telnet_connect()

Connect to telnet server.

**Prototype:**
```c
int telnet_connect(telnet_t *telnet);
```

**Parameters:**
- `telnet`: Telnet structure

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
if (telnet_connect(&telnet) < 0) {
    MB_LOG_ERROR("Connection failed");
    return -1;
}
```

**Implementation Details:**
- Creates non-blocking TCP socket
- Performs DNS resolution if needed
- Initiates connection (may return EINPROGRESS)
- Use `select()` to wait for connection completion

---

### telnet_disconnect()

Disconnect from telnet server.

**Prototype:**
```c
void telnet_disconnect(telnet_t *telnet);
```

**Parameters:**
- `telnet`: Telnet structure

**Example:**
```c
telnet_disconnect(&telnet);
```

---

### telnet_send()

Send data to telnet server (with IAC escaping).

**Prototype:**
```c
ssize_t telnet_send(telnet_t *telnet, const void *data, size_t len);
```

**Parameters:**
- `telnet`: Telnet structure
- `data`: Data to send
- `len`: Length of data

**Returns:**
- Number of bytes sent (>= 0)
- -1 on error

**Example:**
```c
ssize_t n = telnet_send(&telnet, buffer, len);
if (n < 0) {
    MB_LOG_ERROR("Send failed");
}
```

**Implementation Details:**
- Escapes 0xFF bytes as 0xFF 0xFF (IAC escaping)
- Sends via `send()` system call
- Non-blocking operation

---

### telnet_recv()

Receive data from telnet server.

**Prototype:**
```c
ssize_t telnet_recv(telnet_t *telnet, void *buf, size_t len);
```

**Parameters:**
- `telnet`: Telnet structure
- `buf`: Buffer to receive data
- `len`: Buffer size

**Returns:**
- Number of bytes received (>= 0)
- -1 on error
- 0 on connection closed

**Example:**
```c
char buffer[1024];
ssize_t n = telnet_recv(&telnet, buffer, sizeof(buffer));
if (n > 0) {
    /* Process data */
} else if (n == 0) {
    /* Connection closed */
} else if (errno != EAGAIN) {
    /* Error */
}
```

---

### telnet_process_input()

Process received data, handling IAC sequences.

**Prototype:**
```c
ssize_t telnet_process_input(telnet_t *telnet, const char *input, size_t input_len,
                             char *output, size_t output_size);
```

**Parameters:**
- `telnet`: Telnet structure
- `input`: Raw data received from server
- `input_len`: Length of input
- `output`: Buffer for processed data (IAC removed)
- `output_size`: Size of output buffer

**Returns:**
- Number of data bytes in output (>= 0)
- -1 on error

**Example:**
```c
char input[1024], output[1024];
ssize_t n_in = telnet_recv(&telnet, input, sizeof(input));
if (n_in > 0) {
    ssize_t n_out = telnet_process_input(&telnet, input, n_in,
                                          output, sizeof(output));
    if (n_out > 0) {
        /* Send output to serial */
        serial_write(fd, output, n_out);
    }
}
```

**Implementation Details:**
- State machine for IAC sequence parsing
- Handles DO/DONT/WILL/WONT negotiations automatically
- Removes IAC sequences from data stream
- Negotiates BINARY, ECHO, SGA options

---

### telnet_prepare_output()

Prepare data for sending (escape IAC bytes).

**Prototype:**
```c
ssize_t telnet_prepare_output(const char *input, size_t input_len,
                              char *output, size_t output_size);
```

**Parameters:**
- `input`: Raw data from modem
- `input_len`: Length of input
- `output`: Buffer for escaped data
- `output_size`: Size of output buffer

**Returns:**
- Number of bytes in output (>= input_len)
- -1 if buffer too small

**Example:**
```c
char input[512], output[1024];
ssize_t n = telnet_prepare_output(input, input_len, output, sizeof(output));
if (n > 0) {
    telnet_send(&telnet, output, n);
}
```

**Implementation Details:**
- Escapes 0xFF → 0xFF 0xFF
- Output buffer must be at least 2× input size

---

## Bridge Engine API (bridge.c)

### bridge_init()

Initialize bridge with configuration.

**Prototype:**
```c
int bridge_init(bridge_t *bridge);
```

**Parameters:**
- `bridge`: Bridge structure (with config already set)

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
bridge_t bridge;
memset(&bridge, 0, sizeof(bridge));
bridge.config = config;

if (bridge_init(&bridge) < 0) {
    MB_LOG_ERROR("Bridge initialization failed");
    return -1;
}
```

**Implementation Details:**
- Opens serial port
- Initializes modem emulation
- Initializes telnet structure
- Opens data log file if enabled
- Performs health check

---

### bridge_cleanup()

Clean up bridge resources.

**Prototype:**
```c
void bridge_cleanup(bridge_t *bridge);
```

**Parameters:**
- `bridge`: Bridge structure

**Example:**
```c
bridge_cleanup(&bridge);
```

---

### bridge_run()

Main event loop (I/O multiplexing).

**Prototype:**
```c
int bridge_run(bridge_t *bridge);
```

**Parameters:**
- `bridge`: Bridge structure

**Returns:**
- 0 on normal exit
- -1 on error

**Example:**
```c
while (keep_running) {
    if (bridge_run(&bridge) < 0) {
        break;
    }
}
```

**Implementation Details:**
- Uses `select()` to multiplex serial and telnet FDs
- 1-second timeout for periodic health checks
- Calls `bridge_handle_serial()` when serial data ready
- Calls `bridge_handle_telnet()` when telnet data ready
- Checks for connection state changes

---

### bridge_handle_serial()

Handle data from serial port.

**Prototype:**
```c
int bridge_handle_serial(bridge_t *bridge);
```

**Parameters:**
- `bridge`: Bridge structure

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
if (FD_ISSET(bridge->serial_fd, &readfds)) {
    bridge_handle_serial(bridge);
}
```

**Implementation Details:**
- Reads from serial port
- Processes via `modem_process_input()`
- If in online mode, sends to telnet
- Logs data if enabled

---

### bridge_handle_telnet()

Handle data from telnet server.

**Prototype:**
```c
int bridge_handle_telnet(bridge_t *bridge);
```

**Parameters:**
- `bridge`: Bridge structure

**Returns:**
- 0 on success
- -1 on error

**Example:**
```c
if (FD_ISSET(bridge->telnet.sockfd, &readfds)) {
    bridge_handle_telnet(bridge);
}
```

**Implementation Details:**
- Reads from telnet socket
- Processes via `telnet_process_input()`
- Sends to serial port
- Logs data if enabled

---

### ansi_filter_modem_to_telnet()

Filter ANSI sequences from modem to telnet.

**Prototype:**
```c
size_t ansi_filter_modem_to_telnet(const char *input, size_t input_len,
                                   char *output, size_t output_size);
```

**Parameters:**
- `input`: Data from modem
- `input_len`: Input length
- `output`: Filtered output buffer
- `output_size`: Output buffer size

**Returns:**
- Number of bytes in output

**Example:**
```c
char filtered[1024];
size_t filtered_len = ansi_filter_modem_to_telnet(data, len,
                                                   filtered, sizeof(filtered));
telnet_send(&telnet, filtered, filtered_len);
```

**Implementation Details:**
- Removes ANSI cursor control sequences (ESC [...])
- Keeps ANSI text formatting (colors, bold, etc.)
- State machine tracks ESC sequence parsing

---

### ansi_passthrough_telnet_to_modem()

Pass ANSI sequences unchanged from telnet to modem.

**Prototype:**
```c
size_t ansi_passthrough_telnet_to_modem(const char *input, size_t input_len,
                                        char *output, size_t output_size);
```

**Parameters:**
- `input`: Data from telnet
- `input_len`: Input length
- `output`: Output buffer
- `output_size`: Output buffer size

**Returns:**
- Number of bytes in output (same as input_len)

**Example:**
```c
char output[1024];
size_t len = ansi_passthrough_telnet_to_modem(data, data_len,
                                               output, sizeof(output));
serial_write(fd, output, len);
```

**Implementation Details:**
- Currently just copies data unchanged
- Placeholder for future filtering if needed

---

## Configuration API (config.c)

### config_load()

Load configuration from file.

**Prototype:**
```c
config_t *config_load(const char *filename);
```

**Parameters:**
- `filename`: Path to configuration file

**Returns:**
- Pointer to allocated config structure
- NULL on error

**Example:**
```c
config_t *config = config_load("modembridge.conf");
if (!config) {
    MB_LOG_ERROR("Failed to load configuration");
    return -1;
}
```

**Implementation Details:**
- Parses KEY=VALUE format
- Ignores comments (lines starting with #)
- Trims whitespace
- Validates values (baudrate, flow control, etc.)
- Falls back to defaults for missing values

---

### config_free()

Free configuration structure.

**Prototype:**
```c
void config_free(config_t *config);
```

**Parameters:**
- `config`: Configuration to free

**Example:**
```c
config_free(config);
```

---

### config_get_string()

Get string value from configuration.

**Prototype:**
```c
const char *config_get_string(config_t *config, const char *key, const char *default_value);
```

**Parameters:**
- `config`: Configuration structure
- `key`: Configuration key
- `default_value`: Default if key not found

**Returns:**
- Configuration value or default

---

### config_get_int()

Get integer value from configuration.

**Prototype:**
```c
int config_get_int(config_t *config, const char *key, int default_value);
```

**Parameters:**
- `config`: Configuration structure
- `key`: Configuration key
- `default_value`: Default if key not found

**Returns:**
- Configuration value or default

---

### config_get_bool()

Get boolean value from configuration.

**Prototype:**
```c
bool config_get_bool(config_t *config, const char *key, bool default_value);
```

**Parameters:**
- `config`: Configuration structure
- `key`: Configuration key
- `default_value`: Default if key not found

**Returns:**
- Configuration value or default (1/true/yes = true, 0/false/no = false)

---

## Common Utilities (common.c)

### string_trim()

Trim whitespace from string.

**Prototype:**
```c
char *string_trim(char *str);
```

**Parameters:**
- `str`: String to trim (modified in-place)

**Returns:**
- Pointer to trimmed string

**Example:**
```c
char line[256] = "  hello  \n";
char *trimmed = string_trim(line);
/* trimmed = "hello" */
```

---

### string_to_upper()

Convert string to uppercase.

**Prototype:**
```c
void string_to_upper(char *str);
```

**Parameters:**
- `str`: String to convert (modified in-place)

**Example:**
```c
char cmd[32] = "at&v";
string_to_upper(cmd);
/* cmd = "AT&V" */
```

---

### baudrate_to_speed()

Convert baudrate to termios speed constant.

**Prototype:**
```c
speed_t baudrate_to_speed(int baudrate);
```

**Parameters:**
- `baudrate`: Baudrate value (300-230400)

**Returns:**
- termios speed constant (B300, B115200, etc.)
- B0 if invalid

**Example:**
```c
speed_t speed = baudrate_to_speed(115200);
/* speed = B115200 */
```

---

### is_utf8_start()

Check if byte is UTF-8 sequence start.

**Prototype:**
```c
bool is_utf8_start(unsigned char byte);
```

**Parameters:**
- `byte`: Byte to check

**Returns:**
- true if UTF-8 start byte, false otherwise

**Example:**
```c
if (is_utf8_start(buffer[i])) {
    int seq_len = utf8_sequence_length(buffer[i]);
    /* Handle UTF-8 sequence */
}
```

---

### utf8_sequence_length()

Get expected length of UTF-8 sequence.

**Prototype:**
```c
int utf8_sequence_length(unsigned char start_byte);
```

**Parameters:**
- `start_byte`: First byte of UTF-8 sequence

**Returns:**
- Sequence length (1-4 bytes)
- 1 if invalid

**Example:**
```c
int len = utf8_sequence_length(0xE2);  /* Returns 3 */
```

---

## Logging System

ModemBridge uses prefixed logging macros to avoid conflicts with syslog constants.

### Logging Macros

```c
MB_LOG_DEBUG(fmt, ...)    /* Debug messages (only in DEBUG builds) */
MB_LOG_INFO(fmt, ...)     /* Informational messages */
MB_LOG_WARNING(fmt, ...)  /* Warning messages */
MB_LOG_ERROR(fmt, ...)    /* Error messages (includes file:line) */
```

**Example:**
```c
MB_LOG_INFO("Starting ModemBridge v%s", VERSION);
MB_LOG_WARNING("Baudrate %d may be unstable", baudrate);
MB_LOG_ERROR("Failed to open serial port: %s", strerror(errno));
MB_LOG_DEBUG("Received %zu bytes from serial", len);
```

### Log Levels

**Foreground Mode (-v flag)**:
- Logs to stderr
- Shows all levels including DEBUG (with -v)

**Daemon Mode (-d flag)**:
- Logs to syslog (LOG_DAEMON facility)
- DEBUG messages only with -v flag

---

## Error Handling

### Return Value Conventions

**Integer Functions:**
- Return 0 on success
- Return -1 on error
- Check errno for system call errors

**Pointer Functions:**
- Return valid pointer on success
- Return NULL on error

**ssize_t Functions:**
- Return byte count (>= 0) on success
- Return -1 on error
- Return 0 on EOF/closed connection

### Error Checking Example

```c
int fd = serial_init(port, baudrate, PARITY_NONE, 8, 1, FLOW_NONE);
if (fd < 0) {
    MB_LOG_ERROR("serial_init failed: %s", strerror(errno));
    return -1;
}

ssize_t n = serial_read(fd, buffer, sizeof(buffer));
if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* No data available, not an error */
    } else {
        MB_LOG_ERROR("serial_read failed: %s", strerror(errno));
        return -1;
    }
} else if (n == 0) {
    /* No data read, but not an error */
}
```

---

## Extension Guide

### Adding a New AT Command

**Step 1**: Add to `modem_process_command()` in `src/modem.c`

```c
/* In modem_process_command() switch statement */
case 'X':  /* New command ATX */
    p++;
    int value = 0;
    if (isdigit(*p)) {
        value = *p - '0';
        p++;
    }
    if (value >= 0 && value <= 4) {
        modem->settings.result_mode = value;
        MB_LOG_INFO("AT command: ATX%d", value);
    } else {
        return modem_send_response(modem, MODEM_RESP_ERROR);
    }
    break;
```

**Step 2**: Add setting to `modem_settings_t` in `include/modem.h`

```c
typedef struct {
    /* ... existing fields ... */
    int result_mode;  /* ATX command setting */
} modem_settings_t;
```

**Step 3**: Initialize in `modem_reset()` in `src/modem.c`

```c
void modem_reset(modem_t *modem) {
    /* ... existing initialization ... */
    modem->settings.result_mode = 4;  /* Default X4 */
}
```

**Step 4**: Document in `docs/AT_COMMANDS.md`

---

### Adding a New Configuration Option

**Step 1**: Add to `config_t` in `include/config.h`

```c
typedef struct {
    /* ... existing fields ... */
    int new_option;
} config_t;
```

**Step 2**: Parse in `config_load()` in `src/config.c`

```c
if (strcmp(key, "NEW_OPTION") == 0) {
    config->new_option = atoi(value);
}
```

**Step 3**: Add default value

```c
config->new_option = 42;  /* Default */
```

**Step 4**: Document in `docs/CONFIGURATION.md`

---

### Adding a New Protocol Filter

**Step 1**: Add function to `src/bridge.c`

```c
size_t my_filter(const char *input, size_t input_len,
                 char *output, size_t output_size)
{
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len && out_pos < output_size; i++) {
        /* Filter logic here */
        if (input[i] != 0x00) {  /* Example: remove NUL bytes */
            output[out_pos++] = input[i];
        }
    }

    return out_pos;
}
```

**Step 2**: Add prototype to `include/bridge.h`

```c
size_t my_filter(const char *input, size_t input_len,
                 char *output, size_t output_size);
```

**Step 3**: Use in data path

```c
/* In bridge_handle_serial() or bridge_handle_telnet() */
size_t filtered_len = my_filter(data, len, filtered, sizeof(filtered));
```

---

### Adding a New S-Register

**Step 1**: Define constant in `include/modem.h`

```c
#define SREG_NEW_REGISTER    20   /* New S-register */
```

**Step 2**: Initialize in `modem_reset()` in `src/modem.c`

```c
modem->settings.s_registers[SREG_NEW_REGISTER] = 100;  /* Default */
```

**Step 3**: Use in code

```c
int value = modem->settings.s_registers[SREG_NEW_REGISTER];
```

**Step 4**: Document in `docs/AT_COMMANDS.md` S-Register table

---

## Best Practices

1. **Always check return values** from system calls
2. **Use MB_LOG_* macros** for logging (not LOG_* directly)
3. **Bounds-check all buffer operations**
4. **Respect UTF-8 multibyte sequences** when splitting buffers
5. **Use non-blocking I/O** throughout
6. **Handle EAGAIN/EWOULDBLOCK** as non-errors
7. **Initialize structures** with memset() before use
8. **Free all allocated resources** in cleanup paths
9. **Document all new functions** with comments
10. **Test with verbose logging** enabled

---

**See Also:**
- [USER_GUIDE.md](USER_GUIDE.md) - User guide
- [CONFIGURATION.md](CONFIGURATION.md) - Configuration reference
- [AT_COMMANDS.md](AT_COMMANDS.md) - AT command reference
- [EXAMPLES.md](EXAMPLES.md) - Usage examples
