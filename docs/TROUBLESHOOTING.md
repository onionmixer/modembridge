# ModemBridge Troubleshooting Guide

## Table of Contents

1. [Quick Diagnostics](#quick-diagnostics)
2. [Common Problems](#common-problems)
3. [Serial Port Issues](#serial-port-issues)
4. [Connection Problems](#connection-problems)
5. [AT Command Issues](#at-command-issues)
6. [Data Transfer Problems](#data-transfer-problems)
7. [Performance Issues](#performance-issues)
8. [Error Messages](#error-messages)
9. [Debugging Techniques](#debugging-techniques)
10. [FAQ](#faq)

## Quick Diagnostics

### Health Check Failed?

When ModemBridge starts, it performs a comprehensive health check. If any component fails, you'll see status indicators:

**Status Meanings:**
- **OK** - Component working correctly
- **WARNING** - Component has issues but may work
- **ERROR** - Component failed, needs attention

**Health Check Components:**
1. **Serial Port** - File system access check
2. **Serial Initialization** - Port configuration
3. **Modem Device** - AT command response test
4. **Telnet Server** - Connection test

### Quick Fix Checklist

Before diving into detailed troubleshooting, try these quick fixes:

```bash
# 1. Check permissions
groups | grep dialout
# If not in dialout group:
sudo usermod -a -G dialout $USER
# Then log out and back in

# 2. Verify serial port exists
ls -l /dev/ttyUSB0

# 3. Check if port is in use
lsof /dev/ttyUSB0

# 4. Test telnet server manually
telnet telehack.com 23

# 5. Run with verbose logging
./build/modembridge -c modembridge.conf -v
```

## Common Problems

### Problem: ModemBridge Won't Start

**Symptom**: Program exits immediately with error message

**Possible Causes:**

1. **Configuration file not found**
   ```
   Error: Could not open config file: modembridge.conf
   ```
   **Solution**:
   - Specify config path: `./build/modembridge -c /path/to/modembridge.conf`
   - Or copy config to current directory
   - Or install to `/etc/modembridge.conf`

2. **Serial port doesn't exist**
   ```
   ERROR: Serial port does not exist: /dev/ttyUSB0
   ```
   **Solution**:
   - Check connected USB devices: `ls -l /dev/tty{USB,ACM}*`
   - Verify USB adapter is plugged in
   - Check dmesg for USB errors: `dmesg | tail -20`
   - Update SERIAL_PORT in config file

3. **Permission denied**
   ```
   ERROR: Failed to open serial port: Permission denied
   ```
   **Solution**:
   - Add user to dialout group: `sudo usermod -a -G dialout $USER`
   - **IMPORTANT**: Log out and back in for changes to take effect
   - Verify: `groups` should show `dialout`
   - Alternative (not recommended): run with sudo

4. **Port already in use**
   ```
   ERROR: Failed to open serial port: Device or resource busy
   ```
   **Solution**:
   - Find process using port: `lsof /dev/ttyUSB0`
   - Kill the process or close the other program
   - Check for other ModemBridge instances: `ps aux | grep modembridge`

---

### Problem: No Response to AT Commands

**Symptom**: Typing `AT` and pressing Enter shows nothing or garbled text

**Diagnostic Steps:**

1. **Check echo mode**
   ```
   # You may not see what you type
   # Try typing blindly:
   ATE1
   # Now try:
   AT
   OK  <- Should see this
   ```

2. **Verify baudrate match**
   - ModemBridge config must match terminal program
   - Common mismatch: config says 115200, terminal using 9600
   - Solution: Update config or terminal to match

3. **Check serial port settings**
   - Data bits: 8
   - Parity: None
   - Stop bits: 1
   - Flow control: None (or match config)

4. **Test with verbose logging**
   ```bash
   ./build/modembridge -v -c modembridge.conf
   ```
   You should see:
   ```
   DEBUG: Received from serial: AT\r
   DEBUG: AT command: AT
   DEBUG: Sending to serial: \r\nOK\r\n
   ```

**Solutions:**

- **No output at all**: Check physical cable connection
- **Garbled text**: Baudrate mismatch - verify both sides match
- **Echo off**: Send `ATE1` to enable echo
- **Quiet mode on**: Send `ATQ0` to enable responses
- **Wrong port**: Verify SERIAL_PORT in config matches connection

---

### Problem: Connection Fails (NO CARRIER)

**Symptom**: `ATA` command returns `NO CARRIER` immediately

**Diagnostic Steps:**

1. **Test telnet server manually**
   ```bash
   telnet telehack.com 23
   ```
   If this fails, the problem is network-related, not ModemBridge.

2. **Check configuration**
   ```ini
   TELNET_HOST=telehack.com
   TELNET_PORT=23
   ```
   Verify host and port are correct.

3. **Check firewall**
   ```bash
   sudo ufw status
   sudo iptables -L
   ```
   Ensure outgoing TCP connections are allowed.

4. **Check DNS resolution**
   ```bash
   nslookup telehack.com
   ping telehack.com
   ```

5. **Enable verbose logging**
   ```bash
   ./build/modembridge -v -c modembridge.conf
   ```
   Look for:
   ```
   ERROR: Failed to connect to telnet server: Connection refused
   ERROR: Failed to connect to telnet server: Network is unreachable
   ERROR: Failed to connect to telnet server: Host is down
   ```

**Solutions:**

- **Connection refused**: Server not running or wrong port
- **Network unreachable**: Check internet connection
- **Host not found**: Check TELNET_HOST spelling or DNS
- **Connection timeout**: Check firewall or try different server

---

## Serial Port Issues

### Issue: Serial Port Not Found

**Error Message:**
```
ERROR: Serial port does not exist: /dev/ttyUSB0
```

**Solutions:**

1. **Find correct port**
   ```bash
   # List all serial ports
   ls -l /dev/tty{USB,ACM,S}*

   # Watch for new devices (plug/unplug adapter)
   watch -n 1 'ls -l /dev/tty{USB,ACM}* 2>/dev/null'

   # Check kernel messages
   dmesg | grep -i "tty\|usb"
   ```

2. **Common port names**
   - `/dev/ttyUSB0` - FTDI, Prolific, CH340 USB-to-Serial
   - `/dev/ttyACM0` - Arduino, CDC-ACM devices
   - `/dev/ttyS0` - Native COM1 (PC serial port)
   - `/dev/pts/X` - Virtual serial (socat, pty)

3. **Driver issues**
   ```bash
   # Check if driver loaded
   lsmod | grep -i "ftdi\|pl2303\|ch341"

   # Load driver if needed
   sudo modprobe ftdi_sio
   sudo modprobe pl2303
   sudo modprobe ch341
   ```

---

### Issue: Permission Denied

**Error Message:**
```
ERROR: Failed to open serial port: Permission denied
```

**Solutions:**

1. **Add user to dialout group** (Recommended)
   ```bash
   sudo usermod -a -G dialout $USER
   ```
   **IMPORTANT**: You MUST log out and back in (or reboot) for this to take effect!

   Verify it worked:
   ```bash
   groups
   # Should show: ... dialout ...
   ```

2. **Check port permissions**
   ```bash
   ls -l /dev/ttyUSB0
   # Should show: crw-rw---- 1 root dialout ...
   ```

3. **Temporary fix (not recommended for production)**
   ```bash
   sudo chmod 666 /dev/ttyUSB0
   # or
   sudo ./build/modembridge -c modembridge.conf
   ```

---

### Issue: Device or Resource Busy

**Error Message:**
```
ERROR: Failed to open serial port: Device or resource busy
```

**Solutions:**

1. **Find process using port**
   ```bash
   lsof /dev/ttyUSB0
   # Shows: COMMAND  PID USER   FD
   ```

2. **Common culprits**
   - Another ModemBridge instance
   - minicom or screen left open
   - ModemManager (Ubuntu)
   - brltty (Braille device daemon)

3. **Kill the process**
   ```bash
   sudo killall modembridge
   sudo killall minicom
   sudo killall screen
   ```

4. **Disable ModemManager** (if interfering)
   ```bash
   sudo systemctl stop ModemManager
   sudo systemctl disable ModemManager
   ```

5. **Blacklist brltty** (if interfering)
   ```bash
   sudo apt remove brltty
   ```

---

### Issue: Garbled Text / Wrong Characters

**Symptom**: Typing `AT` shows `░▒▓` or random characters

**Cause**: Baudrate mismatch between ModemBridge and terminal

**Solutions:**

1. **Verify baudrate in config**
   ```ini
   BAUDRATE=115200
   ```

2. **Match terminal program baudrate**
   - minicom: Ctrl-A then Z, then O (cOnfigure), Serial port setup
   - screen: `screen /dev/ttyUSB0 115200`
   - PuTTY: Connection → Serial → Speed

3. **Common baudrates**
   - Modern: 115200 or 230400
   - Vintage: 9600, 19200, 38400
   - Very old: 300, 1200, 2400

4. **Test with known baudrate**
   ```bash
   # Set to 9600 and test
   minicom -D /dev/ttyUSB0 -b 9600
   ```

---

## Connection Problems

### Issue: Immediate Disconnection

**Symptom**: `CONNECT` appears briefly, then `NO CARRIER`

**Possible Causes:**

1. **DTR drop**
   - Terminal program drops DTR signal
   - With `AT&D2`, this hangs up connection
   - Solution: Configure terminal to assert DTR
   - Or use `AT&D0` (ignore DTR)

2. **Telnet server rejects connection**
   - Server closes connection immediately
   - Check server logs
   - Try telnet manually to see server response

3. **Telnet option negotiation failure**
   - Enable verbose logging to see IAC sequences
   - May need to adjust telnet.c negotiation

4. **Firewall reset**
   - Connection established but firewall kills it
   - Check iptables/ufw logs

**Solutions:**

```bash
# Enable verbose logging to see what's happening
./build/modembridge -v -c modembridge.conf

# Try with DTR ignored
ATE1
AT&D0
ATA

# Check data log
tail -f modembridge.log
```

---

### Issue: Connection Hangs on ATA

**Symptom**: `ATA` command never returns, no `CONNECT` or `NO CARRIER`

**Possible Causes:**

1. **Telnet server not responding**
2. **Network routing issue**
3. **S7 timeout too long**

**Solutions:**

1. **Check S7 register** (carrier wait time)
   ```
   ATS7?
   060
   OK

   # Reduce timeout to 30 seconds
   ATS7=30
   OK
   ```

2. **Test telnet with timeout**
   ```bash
   timeout 10 telnet telehack.com 23
   ```

3. **Check for network issues**
   ```bash
   traceroute telehack.com
   mtr telehack.com
   ```

4. **Enable verbose logging**
   ```bash
   ./build/modembridge -v -c modembridge.conf
   ```
   Look for:
   ```
   DEBUG: Attempting telnet connection to telehack.com:23
   DEBUG: connect() in progress (EINPROGRESS)
   DEBUG: Waiting for connection to complete...
   ```

---

### Issue: Cannot Return to Command Mode

**Symptom**: Typing `+++` does nothing, connection stays online

**Possible Causes:**

1. **Guard time not observed**
   - Must wait 1 second before `+++`
   - Must wait 1 second after `+++`
   - Must not type anything during guard time

2. **Escape character changed**
   - Default is `+` (ASCII 43)
   - Check S2 register: `ATS2?`

3. **Data echoed back**
   - If server echoes `+++`, timing is broken
   - Try longer guard time

**Solutions:**

1. **Proper escape sequence**
   ```
   (wait 1 second - don't type anything)
   +++
   (wait 1 second - don't type anything)
   OK
   ```

2. **Check escape character**
   ```
   ATS2?
   043
   OK
   # 043 = '+' character
   ```

3. **Increase guard time (S12 register)**
   ```
   # Default S12=50 (1 second in 0.02s units)
   ATS12=100
   OK
   # Now guard time is 2 seconds
   ```

4. **Alternative: Close connection from terminal**
   - Just use ATH before connecting
   - Or close terminal program

---

## AT Command Issues

### Issue: AT Commands Not Recognized

**Symptom**: `ERROR` response to valid commands

**Solutions:**

1. **Check command format**
   ```
   AT          ✓ Correct
   at          ✓ Correct (case-insensitive)
   AT<space>   ✗ Wrong (no spaces)
   A T         ✗ Wrong (no spaces)
   ```

2. **Check line endings**
   - Commands must end with CR (carriage return)
   - Most terminals send CR automatically on Enter
   - If using script, send `\r` not just `\n`

3. **Enable echo to see what's sent**
   ```
   ATE1
   OK
   ```

4. **Check verbose logging**
   ```bash
   ./build/modembridge -v -c modembridge.conf
   ```
   Shows exact bytes received.

---

### Issue: S-Register Changes Don't Work

**Symptom**: Setting S-register has no effect

**Solutions:**

1. **Verify setting syntax**
   ```
   ATS0=2      ✓ Correct
   OK

   ATS0?       ✓ Check value
   002
   OK
   ```

2. **Check register range**
   - S0-S15 are implemented
   - Values must be 0-255
   - Some registers have valid ranges (see AT_COMMANDS.md)

3. **Some registers are read-only**
   - S1 (ring counter) - read-only
   - Setting these returns ERROR

4. **Save configuration**
   ```
   AT&W0       # Save to profile 0
   OK
   ```

---

### Issue: AT&V Shows Wrong Settings

**Symptom**: Configuration display doesn't match expected values

**Solutions:**

1. **Check if settings were saved**
   ```
   AT&W0
   OK
   ```

2. **Reset may have occurred**
   - ATZ resets to defaults
   - Power cycle resets everything
   - Use AT&W to save before reset

3. **Factory defaults**
   ```
   AT&F        # Reset to factory defaults
   OK
   AT&V        # View defaults
   ```

---

## Data Transfer Problems

### Issue: Lost Characters

**Symptom**: Some characters missing during data transfer

**Possible Causes:**

1. **Buffer overflow**
   - Data sent faster than serial port can handle
   - Flow control disabled

2. **Serial port overrun**
   - USB-to-Serial adapter buffer full
   - No hardware/software flow control

**Solutions:**

1. **Enable flow control**
   ```ini
   # In modembridge.conf
   FLOW=HARDWARE  # or SOFTWARE
   ```

2. **Reduce baudrate**
   ```ini
   BAUDRATE=57600  # Instead of 115200
   ```

3. **Check USB controller**
   ```bash
   dmesg | grep -i "tty\|overrun\|frame"
   ```

4. **Use better USB-to-Serial adapter**
   - FTDI chipsets generally more reliable
   - Avoid cheap clones

---

### Issue: Extra Characters Inserted

**Symptom**: Random extra characters appear in data stream

**Possible Causes:**

1. **Telnet IAC escaping issue**
2. **ANSI filtering problem**
3. **UTF-8 multibyte character corruption**

**Solutions:**

1. **Enable data logging**
   ```ini
   DATA_LOG_ENABLED=1
   DATA_LOG_FILE=modembridge.log
   ```

2. **Analyze log file**
   ```bash
   hexdump -C modembridge.log | less
   ```
   Look for:
   - `FF FF` - Telnet IAC escaping
   - `1B [` - ANSI escape sequences
   - Broken UTF-8 sequences

3. **Check telnet binary mode**
   - ModemBridge negotiates BINARY mode
   - If server refuses, 8-bit data may be corrupted

4. **Report issue**
   - Include log file excerpt
   - Specify telnet server used
   - Note when corruption occurs

---

### Issue: Data Corruption with Non-ASCII Characters

**Symptom**: Accented characters, emoji, or Unicode corrupted

**Possible Causes:**

1. **Telnet not in binary mode**
2. **Serial port parity enabled**
3. **UTF-8 sequence split across buffers**

**Solutions:**

1. **Verify binary mode negotiation**
   ```bash
   ./build/modembridge -v -c modembridge.conf
   ```
   Should see:
   ```
   DEBUG: Telnet negotiation: IAC WILL BINARY
   DEBUG: Telnet negotiation: IAC DO BINARY
   ```

2. **Check serial port parity**
   ```ini
   BIT_PARITY=NONE  # Must be NONE for 8-bit data
   ```

3. **Enable data logging**
   - Check if UTF-8 sequences are intact
   - Look for split sequences at buffer boundaries

---

## Performance Issues

### Issue: Slow Response Time

**Symptom**: Noticeable delay between typing and echo

**Possible Causes:**

1. **High system load**
2. **Slow telnet server**
3. **Network latency**

**Solutions:**

1. **Check system load**
   ```bash
   top
   htop
   ```

2. **Test network latency**
   ```bash
   ping telehack.com
   ```

3. **Reduce logging overhead**
   - Run without `-v` flag
   - Disable DATA_LOG_ENABLED

4. **Check USB hub**
   - Connect adapter directly to PC
   - Avoid USB 1.1 hubs

---

### Issue: High CPU Usage

**Symptom**: ModemBridge uses excessive CPU

**Possible Causes:**

1. **Verbose logging in production**
2. **Tight I/O loop**
3. **Data logging enabled**

**Solutions:**

1. **Disable verbose logging**
   - Don't use `-v` in production
   - Only enable for debugging

2. **Disable data logging**
   ```ini
   DATA_LOG_ENABLED=0
   ```

3. **Check for I/O errors**
   ```bash
   ./build/modembridge -v -c modembridge.conf
   ```
   Look for repeated errors.

---

## Error Messages

### Serial Port Errors

| Error Message | Meaning | Solution |
|---------------|---------|----------|
| `Permission denied` | No read/write access | Add user to dialout group |
| `No such file or directory` | Port doesn't exist | Check SERIAL_PORT path |
| `Device or resource busy` | Port in use | Close other program using port |
| `Input/output error` | Hardware problem | Check cable, try different port |
| `No such device` | USB device unplugged | Reconnect USB adapter |

### Configuration Errors

| Error Message | Meaning | Solution |
|---------------|---------|----------|
| `Could not open config file` | File not found | Specify correct path with -c |
| `Invalid baudrate` | Unsupported value | Use standard baudrate (115200, etc.) |
| `Invalid FLOW setting` | Unknown flow control | Use NONE, HARDWARE, or SOFTWARE |
| `SERIAL_PORT not specified` | Missing required config | Add SERIAL_PORT to config |

### Telnet Connection Errors

| Error Message | Meaning | Solution |
|---------------|---------|----------|
| `Connection refused` | Server not accepting | Verify server running and port correct |
| `Network unreachable` | No route to host | Check network connection |
| `Host is down` | Server not responding | Try different server or check firewall |
| `Connection timed out` | Server not responding | Check S7 timeout, verify host reachable |

### Modem Errors

| Error Message | Meaning | Solution |
|---------------|---------|----------|
| `Modem did not respond to AT` | No AT response | Check serial connection and baudrate |
| `Buffer overflow` | Data received too fast | Enable flow control |
| `Invalid command` | Unrecognized AT command | Check command syntax |
| `Parameter out of range` | Invalid value | Check command parameter limits |

---

## Debugging Techniques

### Enable Verbose Logging

**Always start here when troubleshooting:**

```bash
./build/modembridge -v -c modembridge.conf
```

This shows:
- Every AT command received
- Every response sent
- Modem state transitions
- Telnet connection events
- Data flow (brief summary)

### Enable Data Logging

**For data corruption or protocol issues:**

```ini
# In modembridge.conf
DATA_LOG_ENABLED=1
DATA_LOG_FILE=modembridge.log
```

View log:
```bash
tail -f modembridge.log

# Or analyze in hex
hexdump -C modembridge.log | less
```

### Use strace for System Calls

**For low-level debugging:**

```bash
strace -o trace.log ./build/modembridge -c modembridge.conf
```

View trace:
```bash
grep -E "read|write|open|close|connect" trace.log
```

### Check Syslog (Daemon Mode)

**When running as daemon:**

```bash
# Real-time logs
tail -f /var/log/syslog | grep modembridge

# Or with journalctl (systemd)
journalctl -u modembridge -f

# View all daemon logs
journalctl -u modembridge --since "1 hour ago"
```

### Serial Port Traffic Monitoring

**Monitor raw serial data with interceptty:**

```bash
sudo apt install interceptty

# Create intercepting port
interceptty /dev/ttyUSB0 /tmp/intercepted

# Update config to use /tmp/intercepted
# Monitor traffic
tail -f /tmp/interceptty.log
```

### Telnet Traffic Analysis

**Capture telnet traffic with tcpdump:**

```bash
sudo tcpdump -i any -A -s 0 'host telehack.com and port 23'
```

### Test with Virtual Serial Ports

**Isolate problems without hardware:**

```bash
# Terminal 1: Create virtual port pair
socat -d -d pty,raw,echo=0,link=/tmp/vmodem0 \
             pty,raw,echo=0,link=/tmp/vmodem1

# Terminal 2: Run ModemBridge
# Set SERIAL_PORT=/tmp/vmodem1 in config
./build/modembridge -v -c modembridge.conf

# Terminal 3: Connect with minicom
minicom -D /tmp/vmodem0 -b 115200
```

---

## FAQ

### Q: Why do I get "Permission denied" even after adding myself to dialout group?

**A:** You must log out and log back in (or reboot) for group membership changes to take effect. Running `newgrp dialout` in the current shell is not sufficient for all programs.

Verify it worked:
```bash
groups
# Should show: ... dialout ...
```

---

### Q: Can I use ModemBridge with Windows?

**A:** ModemBridge is designed for Linux. For Windows:
- Use WSL (Windows Subsystem for Linux) with USB/IP pass-through
- Use a Linux VM with USB pass-through
- Or port the code to Windows (would need significant changes)

---

### Q: Why does my BBS software not detect the modem?

**A:** Ensure:
1. BBS software is configured for correct serial port
2. Baudrate matches ModemBridge config
3. Modem initialization string is compatible
4. DCD/DTR settings are correct (`AT&C1&D2`)

Test manually first:
```bash
minicom -D /dev/ttyUSB0 -b 115200
AT
OK
```

---

### Q: Can I connect multiple terminals to one ModemBridge?

**A:** No. ModemBridge handles one serial connection to one telnet connection. For multiple connections, run multiple ModemBridge instances on different serial ports.

---

### Q: Why do I see `NO CARRIER` immediately after `CONNECT`?

**A:** This usually means:
1. DTR signal dropped (check `AT&D` setting)
2. Telnet server closed connection immediately
3. Terminal program closed port

Enable verbose logging to see exactly what's happening.

---

### Q: How do I simulate "ringing" for BBS host mode?

**A:** Use auto-answer mode:
```
ATS0=2      # Answer after 2 rings
OK
```

ModemBridge will send `RING` messages and auto-answer when connection request arrives.

---

### Q: Can I use ModemBridge to connect to SSH servers?

**A:** No. ModemBridge only supports telnet protocol (RFC 854). SSH is a completely different protocol with encryption, authentication, etc.

For SSH, you would need a different bridge program or use socat:
```bash
socat TCP-LISTEN:23,reuseaddr EXEC:"ssh user@host"
```

---

### Q: Why does verbose logging show different baudrates in CONNECT message?

**A:** The baudrate in the `CONNECT` message is just the serial port baudrate. It has no relation to the telnet connection speed, which uses whatever bandwidth your network provides.

---

### Q: Can I change the escape sequence from `+++` to something else?

**A:** Yes, using the S2 register:
```
ATS2=126    # Use '~' as escape character
OK

# Now escape with:
~~~
OK
```

---

### Q: How do I completely reset ModemBridge to defaults?

**A:** From AT command mode:
```
AT&F        # Factory reset
OK
ATZ         # Reset modem
OK
```

---

### Q: Is there a GUI for ModemBridge?

**A:** No. ModemBridge is a command-line daemon. Use any serial terminal program (minicom, screen, PuTTY, TeraTerm) for the user interface.

---

### Q: Can I use ModemBridge on Raspberry Pi?

**A:** Yes! ModemBridge works great on Raspberry Pi. Just compile and run as normal. Make sure you're in the dialout group.

---

### Q: Why does `make` fail with "command not found"?

**A:** Install build tools:
```bash
sudo apt update
sudo apt install build-essential
```

---

### Q: How do I make ModemBridge start automatically on boot?

**A:** See Phase 17 (Production Deployment) documentation for systemd service configuration (coming soon).

Temporary solution:
```bash
# Add to /etc/rc.local
/path/to/modembridge -d -c /etc/modembridge.conf
```

---

## Getting Help

If this guide doesn't solve your problem:

1. **Enable verbose logging** and data logging
2. **Collect information**:
   - ModemBridge version: `./build/modembridge --version`
   - OS version: `uname -a`
   - Serial adapter chipset: `lsusb`
   - Configuration file contents
   - Log excerpts showing the problem
   - Exact steps to reproduce

3. **Check existing issues** on GitHub
4. **Open a new issue** with collected information

---

**See Also:**
- [USER_GUIDE.md](USER_GUIDE.md) - Basic usage instructions
- [AT_COMMANDS.md](AT_COMMANDS.md) - AT command reference
- [CONFIGURATION.md](CONFIGURATION.md) - Configuration options
- [EXAMPLES.md](EXAMPLES.md) - Usage examples
