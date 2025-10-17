# Configuration Guide

Complete guide to configuring ModemBridge for your specific needs.

## Table of Contents

1. [Overview](#overview)
2. [Configuration File Format](#configuration-file-format)
3. [Configuration Parameters](#configuration-parameters)
4. [Common Configurations](#common-configurations)
5. [Advanced Settings](#advanced-settings)
6. [Performance Tuning](#performance-tuning)

## Overview

ModemBridge uses a simple key-value configuration file format. The default configuration file is named `modembridge.conf`.

### Configuration File Location

ModemBridge searches for configuration files in this order:

1. **Command line**: File specified with `-c` option
   ```bash
   modembridge -c /path/to/custom.conf
   ```

2. **Current directory**: `./modembridge.conf`
   ```bash
   modembridge  # Looks for ./modembridge.conf
   ```

3. **System directory**: `/etc/modembridge.conf`
   ```bash
   sudo modembridge  # Uses /etc/modembridge.conf
   ```

### Basic Configuration Example

```ini
# Serial Port
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Modem Commands
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X3 S7=60 S10=7"
MODEM_AUTOANSWER_COMMAND="ATS0=2"

# Telnet Server
TELNET_HOST=telehack.com
TELNET_PORT=23

# Data Logging
DATA_LOG_ENABLED=1
DATA_LOG_FILE=modembridge.log
```

## Configuration File Format

### Syntax Rules

1. **Key-Value Pairs**: `KEY=VALUE`
2. **Comments**: Lines starting with `#` are ignored
3. **Whitespace**: Spaces around `=` are trimmed
4. **Quotes**: Values can be quoted: `KEY="value"`
5. **Case Sensitive**: Keys are case-insensitive, values depend on context

### Example

```ini
# This is a comment
SERIAL_PORT=/dev/ttyUSB0      # Inline comments are NOT supported
BAUDRATE=115200               # Numeric value
TELNET_HOST="bbs.example.com" # Quoted string
```

## Configuration Parameters

### Serial Port Configuration

#### SERIAL_PORT

**Type**: String (path)
**Required**: Yes
**Default**: None

Serial device path for USB-to-Serial adapter or native serial port.

**Examples**:
```ini
# Linux USB-to-Serial (FTDI, Prolific, CH340)
SERIAL_PORT=/dev/ttyUSB0

# Linux Arduino or CDC-ACM device
SERIAL_PORT=/dev/ttyACM0

# Linux native serial port
SERIAL_PORT=/dev/ttyS0

# Virtual serial port (for testing)
SERIAL_PORT=/tmp/vmodem
```

**Finding your serial port**:
```bash
# List all serial devices
ls -l /dev/tty{USB,ACM,S}*

# Watch for new devices
dmesg | grep tty
```

---

#### BAUDRATE

**Type**: Integer
**Required**: Yes
**Default**: 9600
**Valid Values**: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400

Communication speed in bits per second.

**Common Values**:
```ini
BAUDRATE=9600    # Standard modem speed
BAUDRATE=19200   # Fast modem speed
BAUDRATE=38400   # Very fast modem
BAUDRATE=57600   # USB-Serial default
BAUDRATE=115200  # Maximum reliable speed (recommended)
BAUDRATE=230400  # Maximum supported speed
```

**Note**: Baudrate must match your terminal program settings exactly!

---

#### BIT_PARITY

**Type**: String
**Required**: No
**Default**: NONE
**Valid Values**: NONE, EVEN, ODD

Parity bit for error detection.

```ini
BIT_PARITY=NONE  # No parity (8N1 - recommended)
BIT_PARITY=EVEN  # Even parity (8E1)
BIT_PARITY=ODD   # Odd parity (8O1)
```

**Standard**: 8N1 (8 data bits, no parity, 1 stop bit) is most common.

---

#### BIT_DATA

**Type**: Integer
**Required**: No
**Default**: 8
**Valid Values**: 5, 6, 7, 8

Number of data bits per character.

```ini
BIT_DATA=8  # Standard (recommended)
BIT_DATA=7  # Old terminals, some ASCII-only systems
```

---

#### BIT_STOP

**Type**: Integer
**Required**: No
**Default**: 1
**Valid Values**: 1, 2

Number of stop bits.

```ini
BIT_STOP=1  # Standard (recommended)
BIT_STOP=2  # Slower, more reliable for noisy lines
```

---

#### FLOW

**Type**: String
**Required**: No
**Default**: NONE
**Valid Values**: NONE, XONXOFF, RTSCTS, BOTH

Flow control method.

```ini
FLOW=NONE     # No flow control (recommended for modern USB)
FLOW=XONXOFF  # Software flow control (XON/XOFF)
FLOW=RTSCTS   # Hardware flow control (RTS/CTS)
FLOW=BOTH     # Both software and hardware
```

**Recommendations**:
- **Modern USB-Serial**: Use `NONE`
- **Long cables/slow speeds**: Use `RTSCTS`
- **ASCII terminals**: Use `XONXOFF`

---

### Modem Configuration

#### MODEM_INIT_COMMAND

**Type**: String (AT commands)
**Required**: No
**Default**: Empty

AT commands executed during health check at startup. Can include `H0` (hang up) to reset modem state.

**Format**: Multiple commands separated by `;`

```ini
# Recommended for BBS host mode
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X3 S7=60 S10=7"

# Minimal initialization
MODEM_INIT_COMMAND="ATH0"

# Full initialization with factory reset
MODEM_INIT_COMMAND="ATH0; AT&F; AT&C1 &D2 E1 V1 Q0 X4"
```

**Command Breakdown**:
- `ATH0` - Hang up (reset connection state)
- `AT&C1` - DCD follows carrier
- `AT&D2` - DTR controls hangup
- `ATB0` - CCITT mode
- `ATX3` - Extended result codes
- `ATS7=60` - Wait 60 seconds for carrier
- `ATS10=7` - Carrier loss delay 0.7 seconds

---

#### MODEM_AUTOANSWER_COMMAND

**Type**: String (AT commands)
**Required**: No
**Default**: Empty

AT commands executed after server starts. Used to enable auto-answer mode for BBS hosting. **MUST NOT include H0** to avoid hanging up on active connections.

```ini
# Auto-answer after 2 rings (BBS host mode)
MODEM_AUTOANSWER_COMMAND="ATS0=2"

# Auto-answer after 1 ring
MODEM_AUTOANSWER_COMMAND="ATS0=1"

# Disable auto-answer (manual ATA required)
MODEM_AUTOANSWER_COMMAND="ATS0=0"

# Auto-answer with additional settings
MODEM_AUTOANSWER_COMMAND="ATS0=2 S7=60"
```

**Warning**: Do NOT include `H0` in this command! It will cause disconnections.

**Good**:
```ini
MODEM_AUTOANSWER_COMMAND="ATS0=2"
```

**Bad**:
```ini
MODEM_AUTOANSWER_COMMAND="ATH0 S0=2"  # DON'T DO THIS!
```

---

#### Deprecated: MODEM_COMMAND

**Status**: Deprecated (use MODEM_INIT_COMMAND instead)

Old configuration parameter. Still supported for backward compatibility but will be removed in future versions.

---

### Telnet Server Configuration

#### TELNET_HOST

**Type**: String (hostname or IP)
**Required**: Yes
**Default**: None

Hostname or IP address of the telnet server to connect to.

```ini
# Public BBS
TELNET_HOST=telehack.com

# IP address
TELNET_HOST=192.168.1.100

# Localhost (for local BBS)
TELNET_HOST=localhost
TELNET_HOST=127.0.0.1
```

---

#### TELNET_PORT

**Type**: Integer
**Required**: Yes
**Default**: 23
**Valid Range**: 1-65535

TCP port number for telnet connection.

```ini
# Standard telnet port
TELNET_PORT=23

# Custom BBS port
TELNET_PORT=2323

# SSH is NOT supported (telnet only)
TELNET_PORT=22  # This won't work!
```

---

### Data Logging Configuration

#### DATA_LOG_ENABLED

**Type**: Integer (boolean)
**Required**: No
**Default**: 0
**Valid Values**: 0 (disabled), 1 (enabled)

Enable hex dump logging of all data transfers.

```ini
DATA_LOG_ENABLED=0  # Logging disabled
DATA_LOG_ENABLED=1  # Logging enabled
```

**Note**: Logging creates detailed hex dumps useful for debugging but increases disk I/O.

---

#### DATA_LOG_FILE

**Type**: String (path)
**Required**: If DATA_LOG_ENABLED=1
**Default**: modembridge.log

Path to log file for data dumps.

```ini
# Relative path (current directory)
DATA_LOG_FILE=modembridge.log

# Absolute path
DATA_LOG_FILE=/var/log/modembridge/data.log

# Different file per session
DATA_LOG_FILE=session-$(date +%Y%m%d).log
```

**Log Format**:
```
[2025-01-15 10:30:45][from_modem] 41 54 0D | AT.
[2025-01-15 10:30:45][to_modem] 0D 0A 4F 4B 0D 0A | ..OK..
```

**Directions**:
- `from_modem` - Data from serial port
- `to_modem` - Data to serial port
- `from_telnet` - Data from telnet server
- `to_telnet` - Data to telnet server
- `internal` - Internal control messages

---

## Common Configurations

### Configuration 1: BBS Client (Dial Out)

Connect from vintage computer to modern BBS via telnet.

**modembridge-client.conf**:
```ini
# Serial Port (USB-to-Serial adapter)
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Initialize modem (reset state)
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4"

# No auto-answer (manual ATA required)
MODEM_AUTOANSWER_COMMAND="ATS0=0"

# Target BBS
TELNET_HOST=telehack.com
TELNET_PORT=23

# Optional: Enable logging for debugging
DATA_LOG_ENABLED=0
```

**Usage**:
```
Terminal Program → Serial → ModemBridge → Telnet → BBS
User types ATA to connect
```

---

### Configuration 2: BBS Host (Auto-Answer)

Run BBS software that answers incoming connections.

**modembridge-host.conf**:
```ini
# Serial Port
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Full modem initialization with H0 to reset
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X3 S7=60 S10=7"

# Auto-answer after 2 rings (NO H0!)
MODEM_AUTOANSWER_COMMAND="ATS0=2"

# Local BBS telnet interface
TELNET_HOST=localhost
TELNET_PORT=2323

# Enable logging to track connections
DATA_LOG_ENABLED=1
DATA_LOG_FILE=/var/log/modembridge/bbs-host.log
```

**Usage**:
```
Telnet Client → Telnet → ModemBridge → Serial → BBS Software
BBS sees it as dialup modem connection
```

---

### Configuration 3: Testing with Virtual Ports

Test without real hardware using socat virtual serial ports.

**modembridge-test.conf**:
```ini
# Virtual serial port (created by socat)
SERIAL_PORT=/tmp/vmodem1
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Minimal initialization
MODEM_INIT_COMMAND="ATH0"
MODEM_AUTOANSWER_COMMAND=""

# Public test server
TELNET_HOST=telehack.com
TELNET_PORT=23

# Enable logging for analysis
DATA_LOG_ENABLED=1
DATA_LOG_FILE=test.log
```

**Setup**:
```bash
# Terminal 1: Create virtual port pair
socat -d -d pty,raw,echo=0,link=/tmp/vmodem0 \
             pty,raw,echo=0,link=/tmp/vmodem1

# Terminal 2: Run ModemBridge
modembridge -c modembridge-test.conf -v

# Terminal 3: Connect with minicom
minicom -D /tmp/vmodem0 -b 115200
```

---

### Configuration 4: High-Speed Connection

Maximum performance for modern USB-Serial adapters.

**modembridge-fast.conf**:
```ini
# Serial Port
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=230400  # Maximum speed
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE        # No flow control for speed

# Minimal initialization
MODEM_INIT_COMMAND="AT&C1 &D2 X4"
MODEM_AUTOANSWER_COMMAND=""

# High-speed BBS
TELNET_HOST=fast-bbs.example.com
TELNET_PORT=23

# Disable logging for maximum speed
DATA_LOG_ENABLED=0
```

---

### Configuration 5: Reliable Connection (Slow)

For noisy lines or unreliable connections.

**modembridge-reliable.conf**:
```ini
# Serial Port
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=9600    # Slower, more reliable
BIT_PARITY=EVEN  # Add parity checking
BIT_DATA=8
BIT_STOP=2       # Two stop bits
FLOW=RTSCTS      # Hardware flow control

# Extended timeouts
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 S7=90 S10=14"
MODEM_AUTOANSWER_COMMAND=""

# Target server
TELNET_HOST=bbs.example.com
TELNET_PORT=23

# Enable logging to debug issues
DATA_LOG_ENABLED=1
DATA_LOG_FILE=reliable.log
```

---

## Advanced Settings

### Multiple Configuration Files

Create different configs for different BBS systems:

**telehack.conf**:
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=telehack.com
TELNET_PORT=23
```

**level29.conf**:
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=level29.org
TELNET_PORT=9999
```

**Run with specific config**:
```bash
modembridge -c telehack.conf
modembridge -c level29.conf
```

---

### Environment Variables

You can use environment variables in config files (future feature):

```ini
SERIAL_PORT=${MODEM_PORT:-/dev/ttyUSB0}
TELNET_HOST=${BBS_HOST:-localhost}
```

*Note: Not currently implemented, but planned for future release.*

---

## Performance Tuning

### For Maximum Speed

```ini
BAUDRATE=230400          # Fastest supported
FLOW=NONE                # No flow control overhead
DATA_LOG_ENABLED=0       # Disable logging
```

**Use when**:
- Modern USB-Serial adapter
- Short cables (< 10 feet)
- Good quality connectors
- Testing with virtual ports

---

### For Maximum Reliability

```ini
BAUDRATE=9600            # Slower speed
BIT_PARITY=EVEN          # Error detection
BIT_STOP=2               # Extra stop bit
FLOW=RTSCTS              # Hardware flow control
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 S7=90 S10=14"  # Long timeouts
DATA_LOG_ENABLED=1       # Log for debugging
```

**Use when**:
- Long cables (> 10 feet)
- Noisy electrical environment
- Old/unreliable hardware
- Intermittent connection issues

---

### For BBS Host Operation

```ini
BAUDRATE=115200          # Good balance
FLOW=NONE                # Simple setup
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X3 S7=60 S10=7"  # Full init
MODEM_AUTOANSWER_COMMAND="ATS0=2"  # Auto-answer enabled
DATA_LOG_ENABLED=1       # Track connections
```

**Critical settings for BBS hosting**:
- `AT&C1` - DCD follows carrier (BBS detects disconnections)
- `AT&D2` - DTR controls hangup (clean disconnect)
- `S7=60` - 60 second carrier wait (time for handshake)
- `S10=7` - 0.7 second carrier loss delay (tolerance)
- `S0=2` - Auto-answer after 2 rings

---

## Configuration Validation

### Check Configuration Syntax

ModemBridge validates configuration at startup. Watch for errors:

```bash
modembridge -c myconfig.conf -v
```

**Common errors**:
```
ERROR: Invalid BAUDRATE: 999999 (not supported)
ERROR: SERIAL_PORT not found: /dev/ttyUSB9
ERROR: Invalid BIT_PARITY: MAYBE (use NONE, EVEN, or ODD)
WARNING: TELNET_PORT out of range: 99999 (using 23)
```

---

### Test Configuration

**Basic test**:
```bash
# Start ModemBridge
modembridge -c myconfig.conf -v

# In another terminal, connect with minicom
minicom -D /dev/ttyUSB0 -b 115200

# Type AT commands:
AT       # Should respond: OK
ATI      # Should show version
AT&V     # Should show configuration
ATA      # Should connect if telnet server is available
```

---

## Troubleshooting Configuration

### Serial Port Issues

**Problem**: Permission denied on serial port

**Solution**:
```bash
# Check permissions
ls -l /dev/ttyUSB0

# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and back in!
```

---

**Problem**: Serial port not found

**Solution**:
```bash
# List all serial devices
ls -l /dev/tty{USB,ACM,S}*

# Watch for device when plugging in
dmesg -w

# Update SERIAL_PORT in config
```

---

### Telnet Connection Issues

**Problem**: Connection refused

**Solution**:
```bash
# Test telnet manually
telnet hostname port

# Check if server is reachable
ping hostname

# Check firewall
sudo iptables -L | grep <port>
```

---

**Problem**: Connection timeout

**Solution**:
- Verify TELNET_HOST spelling
- Check TELNET_PORT number
- Ensure network connectivity
- Try different server to isolate problem

---

## Configuration Best Practices

1. **Start Simple**: Begin with minimal config, add complexity as needed
2. **Test Changes**: Test each configuration change individually
3. **Comment Your Config**: Document why you set specific values
4. **Keep Backups**: Save working configurations
5. **Use Descriptive Names**: Name configs by purpose (bbs-host.conf, test.conf)
6. **Enable Logging Initially**: Turn on logging when testing new configs
7. **Match Terminal Settings**: Ensure baudrate/parity match terminal exactly
8. **Check Health Report**: Review health check output at startup

---

**See Also**:
- [USER_GUIDE.md](USER_GUIDE.md) - General usage
- [AT_COMMANDS.md](AT_COMMANDS.md) - AT command reference
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - Problem solving
- [EXAMPLES.md](EXAMPLES.md) - Usage scenarios
