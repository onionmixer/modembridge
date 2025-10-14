# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ModemBridge is a C-based bridge server that connects dialup modem clients (via USB serial) to telnet servers. It emulates a Hayes-compatible modem, translating AT commands from serial clients into telnet connections, enabling legacy dialup software to connect to modern telnet services.

**Key Characteristics:**
- Pure C implementation (C11/GNU11)
- Zero external dependencies beyond glibc and POSIX APIs
- Daemon-capable with syslog integration
- Minimum requirement: Ubuntu 22.04 LTS

## Build Commands

```bash
# Standard build
make

# Clean build
make clean && make

# Debug build (with symbols, no optimization)
make debug
# or
make DEBUG=1

# Run with local config
make run

# Install system-wide (requires root)
sudo make install

# Uninstall
sudo make uninstall
```

## Running the Application

```bash
# Foreground with config file
./build/modembridge -c modembridge.conf

# Daemon mode with verbose logging
./build/modembridge -c modembridge.conf -d -v

# View help
./build/modembridge --help
```

The daemon uses syslog for logging. View logs with:
```bash
journalctl -u modembridge -f
# or
tail -f /var/log/syslog | grep modembridge
```

## Architecture Overview

### Layered Design

The codebase follows a layered architecture with clear separation of concerns:

```
┌─────────────────────────────────────┐
│         main.c (Daemon)             │  Signal handling, main loop
├─────────────────────────────────────┤
│       bridge.c (Core Engine)        │  I/O multiplexing, data routing
├──────────────┬──────────────────────┤
│   modem.c    │     telnet.c         │  Protocol layers
│ (AT Commands)│  (RFC 854, IAC)      │
├──────────────┴──────────────────────┤
│   serial.c (termios/POSIX)          │  Hardware abstraction
├─────────────────────────────────────┤
│   config.c, common.c (Utils)        │  Support infrastructure
└─────────────────────────────────────┘
```

### Critical Data Flow

**Serial → Telnet Path:**
1. `serial_read()` receives raw bytes from USB serial port
2. `modem_process_input()` handles AT commands (command mode) or detects escape sequences (online mode)
3. `ansi_filter_modem_to_telnet()` removes ANSI cursor control codes (but keeps text styling)
4. `telnet_prepare_output()` escapes IAC bytes (0xFF → 0xFF 0xFF)
5. `telnet_send()` transmits to telnet server

**Telnet → Serial Path:**
1. `telnet_recv()` receives data from telnet server
2. `telnet_process_input()` parses IAC sequences, handles option negotiation, returns clean data
3. `ansi_passthrough_telnet_to_modem()` passes ANSI codes unchanged
4. `serial_write()` sends to modem client

### State Management

The bridge operates in multiple concurrent state machines:

**Modem States** (modem.h):
- `MODEM_STATE_COMMAND`: Processing AT commands
- `MODEM_STATE_ONLINE`: Transparent data mode
- `MODEM_STATE_CONNECTING`: Establishing telnet connection
- `MODEM_STATE_DISCONNECTED`: Idle

**Connection States** (common.h):
- `STATE_IDLE`: No active connection
- `STATE_CONNECTED`: Bridging active between serial and telnet
- `STATE_DISCONNECTING`: Teardown in progress

**Telnet Protocol State** (telnet.h):
- State machine in `telnet_process_input()` for IAC sequence parsing
- Tracks `TELNET_STATE_DATA`, `TELNET_STATE_IAC`, `TELNET_STATE_WILL/WONT/DO/DONT`, etc.

### Key Design Patterns

**I/O Multiplexing:**
- `bridge_run()` uses `select()` to monitor both serial FD and telnet socket FD
- Non-blocking I/O throughout for responsive handling
- 1-second timeout allows periodic health checks and signal processing

**Escape Sequence Detection:**
- `modem_process_input()` implements the Hayes escape sequence (+++ with guard time)
- Tracks escape character count and timing to switch from online → command mode

**Protocol Translation:**
- Telnet IAC escaping is bidirectional but asymmetric
- ANSI filtering is unidirectional (only modem → telnet removes cursor codes)
- AT command responses respect verbose/quiet mode settings

## Important Implementation Details

### Logging Macros

All logging uses prefixed macros to avoid conflicts with syslog.h constants:
- `MB_LOG_DEBUG()` - only active in DEBUG builds
- `MB_LOG_INFO()` - standard operational messages
- `MB_LOG_WARNING()` - non-critical issues
- `MB_LOG_ERROR()` - errors with context (file:line)

Never use `LOG_*` macros directly as they conflict with syslog priority constants.

### Serial Port Configuration

Serial port setup in `serial.c` uses POSIX termios:
- Must save original settings (`oldtio`) for restoration on close
- Raw mode: disable ICANON, ECHO, ISIG, and all input/output processing
- Non-blocking: `VMIN=0, VTIME=0` with `O_NONBLOCK` flag
- Flow control implemented via CRTSCTS (hardware) or IXON/IXOFF (software)

Dynamic baudrate changes are supported via `serial_set_baudrate()` for adaptive modem negotiation.

### Telnet Protocol Compliance

The telnet implementation (`telnet.c`) must:
- Always escape 0xFF bytes as 0xFF 0xFF when sending data
- Negotiate BINARY mode for UTF-8/multibyte character support
- Respond to DO/DONT/WILL/WONT within the state machine (automatic handling)
- Support both linemode and character mode transparently

Current implementation accepts BINARY and SGA options, rejects others.

### ANSI Sequence Handling

The ANSI filter (`bridge.c`) uses a state machine:
- `ANSI_STATE_NORMAL` → `ANSI_STATE_ESC` (on 0x1B) → `ANSI_STATE_CSI` (on '[')
- CSI parameters (0x30-0x3F) accumulate in `ANSI_STATE_CSI_PARAM`
- Final byte (0x40-0x7E) terminates sequence and returns to `ANSI_STATE_NORMAL`

This removes cursor movement and screen control from modem → telnet, allowing text formatting to pass through.

### Configuration File Format

`modembridge.conf` uses simple `KEY=VALUE` format:
- Lines starting with `#` are comments
- Values can be quoted: `TELNET_HOST="127.0.0.1"`
- Parser in `config.c` trims whitespace and validates types
- Invalid values fall back to safe defaults with warnings

Supported baudrates: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400

## Testing Notes

The application requires actual hardware or virtual devices for meaningful testing:

**Serial Port Testing:**
- Use `socat` to create virtual serial port pairs:
  ```bash
  socat -d -d pty,raw,echo=0 pty,raw,echo=0
  # Connect one end to modembridge, use minicom on the other
  ```

**Telnet Server:**
- Any telnet service will work for testing
- For local testing, use `busybox telnetd` or `xinetd` with telnet config

**AT Command Testing:**
- Connect via terminal emulator (minicom, screen, cu)
- Test commands: `AT`, `ATZ`, `ATE1`, `ATDT`, `ATA`, `+++`, `ATH`
- Expected responses: `OK`, `CONNECT 57600`, `NO CARRIER`

## Code Modification Guidelines

**When adding new AT commands:**
- Add handling in `modem_process_command()` switch statement
- Update S-registers if new settings are needed (see `SREG_*` constants)
- Respect echo/verbose/quiet mode settings when sending responses

**When modifying protocol handlers:**
- Serial changes: maintain non-blocking semantics
- Telnet changes: ensure IAC escaping remains correct
- Bridge changes: preserve multibyte character boundaries

**Multibyte Character Safety:**
UTF-8 helper functions in `bridge.c`:
- `is_utf8_start()` - detects UTF-8 sequence start bytes
- `is_utf8_continuation()` - identifies continuation bytes
- `utf8_sequence_length()` - calculates expected sequence length
- Use these to avoid splitting multibyte characters across buffer boundaries

## Common Issues

**Compilation errors about LOG_* macros:**
- Use `MB_LOG_*` macros instead of `LOG_*`
- Include `<syslog.h>` before defining logging macros

**Serial port permission denied:**
- Add user to dialout group: `sudo usermod -a -G dialout $USER`
- Or run with sudo (not recommended for production)

**Telnet connection fails immediately:**
- Check TELNET_HOST and TELNET_PORT in config
- Verify telnet server is listening: `netstat -tln | grep 8882`
- Check firewall rules if connecting to remote host

**Modem doesn't respond to AT commands:**
- Ensure serial port settings match (baudrate, parity, flow control)
- Check cable wiring (TX/RX crossed correctly for null modem)
- Enable verbose logging (`-v`) to see AT command processing
