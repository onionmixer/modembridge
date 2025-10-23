# ModemBridge User Guide

## Table of Contents

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [Installation](#installation)
4. [Configuration](#configuration)
5. [Running ModemBridge](#running-modembridge)
6. [Connecting Your Terminal](#connecting-your-terminal)
7. [Using AT Commands](#using-at-commands)
8. [Common Usage Scenarios](#common-usage-scenarios)
9. [Tips and Best Practices](#tips-and-best-practices)

## Introduction

ModemBridge is a software modem emulator that allows vintage computers, terminal programs, and BBS software to connect to modern telnet servers through a serial port connection. It emulates a Hayes-compatible dialup modem, translating AT commands into telnet connections.

### What Can You Do with ModemBridge?

- **Connect vintage computers to modern BBS systems** via telnet
- **Use classic terminal programs** (Procomm, Telix, Qmodem, etc.) with telnet services
- **Run BBS software** in host mode with auto-answer support
- **Bridge serial devices** to network services
- **Test retro communication software** without actual phone lines

### System Requirements

- **Operating System**: Linux (Ubuntu 22.04 LTS or newer)
- **Serial Port**: USB-to-Serial adapter or native serial port
- **Permissions**: Read/write access to serial device
- **Network**: TCP/IP connectivity

## Getting Started

### Quick Start (5 Minutes)

1. **Build ModemBridge**
   ```bash
   cd modembridge
   make
   ```

2. **Configure your serial port**
   ```bash
   # Edit modembridge.conf
   SERIAL_PORT=/dev/ttyUSB0
   BAUDRATE=115200
   TELNET_HOST=telehack.com
   TELNET_PORT=23
   ```

3. **Add yourself to the dialout group**
   ```bash
   sudo usermod -a -G dialout $USER
   # Log out and back in
   ```

4. **Run ModemBridge**
   ```bash
   ./build/modembridge -c modembridge.conf -v
   ```

5. **Connect with your terminal program**
   - Open your terminal software (PuTTY, minicom, etc.)
   - Connect to /dev/ttyUSB0 at 115200 baud
   - Type `AT` and press Enter - you should see `OK`

## Installation

### Building from Source

```bash
# Clone or download the repository
cd modembridge

# Build the project
make

# The executable will be at: build/modembridge
```

### System-wide Installation (Optional)

```bash
# Install to /usr/local/bin (requires root)
sudo make install

# This allows you to run 'modembridge' from anywhere
modembridge -c /etc/modembridge.conf
```

### Uninstallation

```bash
sudo make uninstall
```

## Configuration

### Configuration File Location

ModemBridge looks for configuration in this order:
1. File specified with `-c` option
2. `./modembridge.conf` (current directory)
3. `/etc/modembridge.conf` (system-wide)

### Basic Configuration

Create `modembridge.conf` with the following content:

```ini
# Serial Port Configuration
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Modem Initialization (executed during health check)
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X3 S7=60 S10=7"

# Modem Auto-Answer (executed after server start, NO H0!)
MODEM_AUTOANSWER_COMMAND="ATS0=2"

# Telnet Server Configuration
TELNET_HOST=telehack.com
TELNET_PORT=23

# Data Logging (optional)
DATA_LOG_ENABLED=1
DATA_LOG_FILE=modembridge.log
```

### Finding Your Serial Port

**Linux:**
```bash
# List all serial devices
ls -l /dev/tty{USB,ACM}*

# Most USB-to-Serial adapters appear as:
# /dev/ttyUSB0  - FTDI, Prolific, CH340 chips
# /dev/ttyACM0  - Arduino, some CDC-ACM devices

# Watch for new devices when plugging in adapter
dmesg | tail -20
```

**Windows (using WSL):**
- COM1 = /dev/ttyS0
- COM3 = /dev/ttyS2
- Check Windows Device Manager for COM port number

### Serial Port Permissions

**Add yourself to dialout group:**
```bash
sudo usermod -a -G dialout $USER
```

**Important**: You must log out and back in for group changes to take effect!

**Verify permissions:**
```bash
groups  # Should show 'dialout' in the list
ls -l /dev/ttyUSB0  # Should show 'crw-rw---- ... root dialout'
```

## Running ModemBridge

### Command Line Options

```
Usage: modembridge [options]

Options:
  -c, --config FILE    Configuration file path
  -d, --daemon         Run as background daemon
  -v, --verbose        Enable verbose (DEBUG) logging
  -h, --help           Show help message
  -V, --version        Show version information
```

### Running in Foreground (Recommended for Testing)

```bash
# Run with default config
./build/modembridge

# Run with custom config and verbose logging
./build/modembridge -c myconfig.conf -v

# This will show real-time activity in your terminal
```

### Running as Daemon (Background Mode)

```bash
# Start as daemon
sudo ./build/modembridge -d -c /etc/modembridge.conf

# Check logs
tail -f /var/log/syslog | grep modembridge

# Stop daemon
sudo killall modembridge
```

### Health Check Output

When ModemBridge starts, it performs a health check:

```
=== Health Check ===

Serial Port:
  Status: OK
  Device exists and accessible: /dev/ttyUSB0

Serial Initialization:
  Status: OK
  Serial port initialized: 115200 baud, 8N1, flow=NONE

Modem Device:
  Status: OK
  Modem responded to AT command

Telnet Server:
  Status: OK
  Connected: telehack.com:23

====================
```

**Status indicators:**
- **OK**: Component is working correctly
- **WARNING**: Component has issues but may work
- **ERROR**: Component failed and needs attention

## Connecting Your Terminal

### Using Minicom (Linux)

```bash
# Install minicom
sudo apt-get install minicom

# Run minicom
minicom -D /dev/ttyUSB0 -b 115200

# Or configure minicom:
minicom -s
# Set Serial Device: /dev/ttyUSB0
# Set Bps/Par/Bits: 115200 8N1
# Set Hardware Flow Control: No
# Set Software Flow Control: No
# Save setup as default
# Exit

# Then just run: minicom
```

### Using Screen (Linux/macOS)

```bash
screen /dev/ttyUSB0 115200

# To exit screen: Ctrl-A then K (kill), then Y (yes)
```

### Using PuTTY (Windows/Linux)

1. Open PuTTY
2. Select "Serial" connection type
3. Serial line: COM3 (or /dev/ttyUSB0 on Linux)
4. Speed: 115200
5. Click "Open"

### Using TeraTerm (Windows)

1. Open TeraTerm
2. File → New Connection
3. Select "Serial"
4. Port: COM3
5. Speed: 115200
6. OK

## Using AT Commands

### Testing the Connection

Once connected, test with basic commands:

```
AT                  → OK
ATE1                → OK (echo on)
ATI                 → ModemBridge v1.0.0
                      OK
AT&V                → (displays current configuration)
```

### Connecting to a Server

```
ATA                 → OK
                      CONNECT 115200
(now you're connected to the telnet server)
```

### Returning to Command Mode

While online (connected):

1. **Wait 1 second** (don't type anything)
2. **Type `+++`** (three plus signs)
3. **Wait 1 second** again
4. You'll see `OK` - now you're in command mode
5. Connection is still active!

### Disconnecting

From command mode:
```
ATH                 → OK
                      NO CARRIER
```

Or just close your terminal program.

### Going Back Online

If you used `+++` to enter command mode but want to resume data:
```
ATO                 → CONNECT
(back to online mode)
```

## Common Usage Scenarios

### Scenario 1: Connecting to a Public BBS

```
1. Start ModemBridge with BBS configuration
   TELNET_HOST=mybbs.example.com
   TELNET_PORT=23

2. Open terminal program

3. Type commands:
   AT                # Test connection
   ATE1              # Enable echo
   ATA               # Connect to BBS

4. You're now connected - enjoy the BBS!

5. To disconnect:
   +++               # (wait 1 sec before and after)
   ATH               # Hang up
```

### Scenario 2: Testing with Multiple BBSs

Create different config files:

**telehack.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=telehack.com
TELNET_PORT=23
```

**level29.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=level29.org
TELNET_PORT=9999
```

Run with different configs:
```bash
# Connect to Telehack
./build/modembridge -c telehack.conf

# Stop and connect to Level 29
./build/modembridge -c level29.conf
```

### Scenario 3: Running a BBS (Host Mode)

For BBS software that needs to answer incoming calls:

**bbs-host.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200

# Initialize modem (with H0 to reset)
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X3 S7=60 S10=7"

# Auto-answer after 2 rings (NO H0 here!)
MODEM_AUTOANSWER_COMMAND="ATS0=2"

TELNET_HOST=localhost
TELNET_PORT=2323
DATA_LOG_ENABLED=1
```

This configuration:
- Resets modem with H0 during startup
- Enables auto-answer (S0=2)
- Connects to local telnet server (your BBS)
- Logs all data for debugging

### Scenario 4: Virtual Serial Ports (Testing)

Test without real hardware using socat:

```bash
# Terminal 1: Create virtual serial port pair
socat -d -d pty,raw,echo=0,link=/tmp/vmodem0 \
             pty,raw,echo=0,link=/tmp/vmodem1

# Terminal 2: Run ModemBridge
# Edit config: SERIAL_PORT=/tmp/vmodem1
./build/modembridge -c modembridge.conf -v

# Terminal 3: Connect with minicom
minicom -D /tmp/vmodem0 -b 115200
```

## Tips and Best Practices

### General Tips

1. **Always test with verbose mode first**
   ```bash
   ./build/modembridge -v -c yourconfig.conf
   ```
   This shows what's happening in real-time.

2. **Match baudrates everywhere**
   - ModemBridge config: `BAUDRATE=115200`
   - Terminal program: 115200 baud
   - They must match exactly!

3. **Disable flow control in terminal**
   - Set Hardware Flow Control: OFF
   - Set Software Flow Control: OFF
   - Unless your serial device requires it

4. **Use data logging for debugging**
   ```ini
   DATA_LOG_ENABLED=1
   DATA_LOG_FILE=modembridge.log
   ```
   View the log to see exactly what's being sent/received.

### AT Command Best Practices

1. **Test connection first**
   ```
   AT        # Should respond OK
   ATI       # Shows version
   AT&V      # Shows all settings
   ```

2. **Enable echo for easier typing**
   ```
   ATE1      # You'll see what you type
   ```

3. **Use verbose mode for readable responses**
   ```
   ATV1      # Responses like "OK" instead of "0"
   ```

4. **Chain commands for efficiency**
   ```
   ATE1V1Q0  # Enable echo, verbose, responses all at once
   ```

### Troubleshooting Quick Checks

**No response to AT commands?**
- Check serial port path in config
- Verify baudrate matches terminal
- Check permissions (dialout group)
- Try verbose mode: `-v`

**Connection fails?**
- Test telnet manually: `telnet hostname port`
- Check firewall settings
- Verify telnet server is running

**Garbled text?**
- Check baudrate (must match exactly)
- Verify parity/data/stop bits (8N1 is standard)
- Try different USB port
- Check cable quality

**See TROUBLESHOOTING.md for detailed solutions**

## Next Steps

- **AT Commands**: See [AT_COMMANDS.md](AT_COMMANDS.md) for complete command reference
- **Configuration**: See [CONFIGURATION.md](CONFIGURATION.md) for advanced options
- **Examples**: See [EXAMPLES.md](EXAMPLES.md) for more usage scenarios
- **Troubleshooting**: See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for problem solving

---

**Happy Retrocomputing!** 🖥️📞
