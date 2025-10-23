# ModemBridge User Guide

## Quick Start (5 Minutes)

### 1. Build ModemBridge
```bash
cd modembridge
make
```

### 2. Configure Serial Port
Create `modembridge.conf`:
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=telehack.com
TELNET_PORT=23
```

### 3. Set Permissions
```bash
sudo usermod -a -G dialout $USER
# Log out and back in
```

### 4. Run ModemBridge
```bash
./build/modembridge -c modembridge.conf -v
```

### 5. Connect Terminal
- Open terminal software (minicom, PuTTY, etc.)
- Connect to /dev/ttyUSB0 at 115200 baud
- Type `AT` - should see `OK`
- Type `ATDT` to connect

## What is ModemBridge?

ModemBridge emulates a Hayes-compatible modem, allowing vintage computers and terminal programs to connect to modern telnet servers through a serial port. It translates AT commands into telnet connections.

### Use Cases
- Connect vintage computers to modern BBS systems
- Use classic terminal programs with telnet services
- Run BBS software with auto-answer support
- Bridge serial devices to network services
- Test retro communication software

### System Requirements
- **OS**: Linux (Ubuntu 22.04 LTS or newer)
- **Port**: USB-to-Serial adapter or native serial port
- **Permissions**: Read/write access to serial device
- **Network**: TCP/IP connectivity

## Installation

### Building from Source
```bash
# Build the project
make

# Optional: Install system-wide
sudo make install

# Uninstall
sudo make uninstall
```

## Basic Usage

### Command Line Options
```
modembridge [options]

Options:
  -c, --config FILE    Configuration file path
  -d, --daemon         Run as background daemon
  -v, --verbose        Enable verbose logging
  -h, --help           Show help message
```

### Health Check Output
When ModemBridge starts successfully:
```
ModemBridge v1.0.0 starting...
Configuration loaded from: modembridge.conf
Serial port: /dev/ttyUSB0 (115200 8N1)
Telnet server: telehack.com:23
Modem initialized with: ATZ
Auto-answer enabled: ATS0=2
Ready for connections
```

### Basic AT Commands
```
AT          - Test command (returns OK)
ATDT        - Connect to configured telnet server
ATA         - Answer incoming connection
ATH         - Hang up current connection
+++         - Return to command mode
ATO         - Return to online mode
ATZ         - Reset modem to defaults
```

For complete AT command reference, see [INFO_AT_COMMANDS.md](INFO_AT_COMMANDS.md)

## Finding Your Serial Port

### Linux
```bash
# List serial devices
ls -l /dev/tty{USB,ACM}*

# Watch for new devices
dmesg | tail -20
```

Common devices:
- `/dev/ttyUSB0` - FTDI, Prolific, CH340 chips
- `/dev/ttyACM0` - Arduino, CDC-ACM devices

### Permissions
```bash
# Add to dialout group
sudo usermod -a -G dialout $USER
# Must log out and back in!

# Verify
groups  # Should show 'dialout'
```

## Terminal Program Setup

### Minicom
```bash
minicom -s
# Serial port setup:
# - Serial Device: /dev/ttyUSB0
# - Bps/Par/Bits: 115200 8N1
# - Hardware Flow: No
# - Software Flow: No
```

### PuTTY
- Connection type: Serial
- Serial line: /dev/ttyUSB0 (or COM3 on Windows)
- Speed: 115200
- Data bits: 8, Stop bits: 1, Parity: None
- Flow control: None

### Screen
```bash
screen /dev/ttyUSB0 115200
# Exit: Ctrl-A then K
```

## Common Scenarios

For detailed configuration examples, see:
- [INFO_CONFIGURATION.md](INFO_CONFIGURATION.md) - Complete configuration reference
- [INFO_EXAMPLES.md](INFO_EXAMPLES.md) - 20+ detailed usage examples

### Quick Examples

**Connect to BBS:**
```
AT
ATDT              # Uses configured server
ATDT bbs.example.com:2323  # Override server
```

**Auto-answer Mode:**
```
ATS0=2            # Answer after 2 rings
AT&W              # Save settings
```

**Return to Command Mode:**
```
+++               # Wait 1 second, don't press Enter
ATH               # Hang up
```

## Troubleshooting

For detailed troubleshooting, see [INFO_TROUBLESHOOTING.md](INFO_TROUBLESHOOTING.md)

### Common Issues

**No response to AT commands:**
- Check serial port and baudrate match
- Verify permissions (`groups` should show dialout)
- Try different terminal program

**Connection fails immediately:**
- Check network connectivity to telnet server
- Verify TELNET_HOST and TELNET_PORT in config
- Check firewall settings

**Garbled text:**
- Baudrate mismatch - verify both sides use same speed
- Try hardware flow control (FLOW=RTS_CTS)
- Check terminal emulation settings

## Tips and Best Practices

1. **Start Simple**: Test with `telehack.com:23` first
2. **Use Verbose Mode**: Run with `-v` for debugging
3. **Check Logs**: Enable DATA_LOG for troubleshooting
4. **Match Baudrates**: Ensure terminal and config match
5. **Test AT Commands**: Verify basic AT/OK before connecting

## Additional Documentation

- [INFO_AT_COMMANDS.md](INFO_AT_COMMANDS.md) - Complete AT command reference
- [INFO_CONFIGURATION.md](INFO_CONFIGURATION.md) - All configuration options
- [INFO_EXAMPLES.md](INFO_EXAMPLES.md) - Detailed usage scenarios
- [INFO_TROUBLESHOOTING.md](INFO_TROUBLESHOOTING.md) - Problem solving guide
- [INFO_TELNET_IMPLEMENTATION.md](INFO_TELNET_IMPLEMENTATION.md) - Technical telnet details

## Support

For issues, check:
1. [Troubleshooting Guide](INFO_TROUBLESHOOTING.md)
2. Configuration examples in [INFO_EXAMPLES.md](INFO_EXAMPLES.md)
3. GitHub Issues page

---
*Version 1.0.0 - Simplified User Guide*
*For detailed information, see linked documentation*