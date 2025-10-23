# ModemBridge Usage Examples

## Table of Contents

1. [Basic Examples](#basic-examples)
2. [BBS Client Scenarios](#bbs-client-scenarios)
3. [BBS Host Scenarios](#bbs-host-scenarios)
4. [Testing and Development](#testing-and-development)
5. [Advanced Configurations](#advanced-configurations)
6. [Scripted Automation](#scripted-automation)
7. [Integration Examples](#integration-examples)

## Basic Examples

### Example 1: First Time Setup

**Goal**: Get ModemBridge running for the first time

**Steps:**

```bash
# 1. Build the project
cd modembridge
make clean && make

# 2. Find your serial port
ls -l /dev/ttyUSB*
# Output: /dev/ttyUSB0

# 3. Create basic configuration
cat > test.conf <<EOF
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=telehack.com
TELNET_PORT=23
EOF

# 4. Add yourself to dialout group
sudo usermod -a -G dialout $USER
# Log out and back in!

# 5. Run ModemBridge with verbose logging
./build/modembridge -c test.conf -v
```

**Expected Output:**
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

Server started. Waiting for connections...
```

**Next Steps:**
- Open terminal program (minicom, PuTTY, etc.)
- Connect to /dev/ttyUSB0 at 115200 baud
- Type `AT` and press Enter
- You should see `OK`

---

### Example 2: Connecting to a Public BBS

**Goal**: Connect to a telnet BBS using ModemBridge

**Configuration (bbs.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

TELNET_HOST=bbs.fozztexx.com
TELNET_PORT=23

DATA_LOG_ENABLED=0
```

**Terminal Session:**
```bash
# Terminal 1: Start ModemBridge
./build/modembridge -c bbs.conf -v

# Terminal 2: Connect with minicom
minicom -D /dev/ttyUSB0 -b 115200
```

**AT Commands:**
```
AT
OK

ATE1V1
OK

ATI
ModemBridge v1.0.0
OK

ATA
OK
CONNECT 115200

[Now you're connected to the BBS - enjoy!]

(To disconnect: Press Ctrl-A, then Q in minicom)
```

---

### Example 3: Quick Connection Test

**Goal**: Test connection to multiple servers quickly

**Script (test-bbses.sh):**
```bash
#!/bin/bash

# List of BBS servers to test
declare -A servers=(
    ["Telehack"]="telehack.com:23"
    ["FozzTexx"]="bbs.fozztexx.com:23"
    ["Level 29"]="level29.org:9999"
)

for name in "${!servers[@]}"; do
    IFS=':' read -r host port <<< "${servers[$name]}"

    echo "Testing $name ($host:$port)..."

    # Create temp config
    cat > /tmp/test.conf <<EOF
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=$host
TELNET_PORT=$port
EOF

    # Test connection
    timeout 5 telnet $host $port && echo "✓ $name: OK" || echo "✗ $name: FAILED"
    echo
done
```

---

## BBS Client Scenarios

### Example 4: Classic BBS Terminal

**Goal**: Emulate a classic dialup terminal for BBS calling

**Configuration (classic-bbs.conf):**
```ini
# Serial port (USB-to-Serial adapter)
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=2400                # Classic 2400 baud modem
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Target BBS
TELNET_HOST=retrobbs.com
TELNET_PORT=23

# Data logging for debugging
DATA_LOG_ENABLED=1
DATA_LOG_FILE=bbs-session.log
```

**Using with Vintage Software (e.g., Telix, Procomm):**

1. **Connect vintage PC to USB-to-Serial adapter**
2. **Start ModemBridge on modern Linux PC**
   ```bash
   ./build/modembridge -c classic-bbs.conf
   ```
3. **In Telix/Procomm on vintage PC:**
   - Set COM port (COM1, COM2, etc.)
   - Set speed to 2400 baud
   - Set 8-N-1 (8 data bits, no parity, 1 stop bit)
   - Disable flow control
4. **Dial command:**
   ```
   ATDT
   OK
   ATA
   OK
   CONNECT 2400
   ```

---

### Example 5: Multi-BBS Configuration

**Goal**: Easily switch between different BBS servers

**Directory Structure:**
```
configs/
├── telehack.conf
├── level29.conf
├── fozztexx.conf
└── local-test.conf
```

**configs/telehack.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=telehack.com
TELNET_PORT=23
```

**configs/level29.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=level29.org
TELNET_PORT=9999
```

**configs/fozztexx.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=bbs.fozztexx.com
TELNET_PORT=23
```

**configs/local-test.conf:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=localhost
TELNET_PORT=2323
```

**Switch Script (switch-bbs.sh):**
```bash
#!/bin/bash

echo "Available BBS systems:"
echo "1) Telehack"
echo "2) Level 29"
echo "3) FozzTexx BBS"
echo "4) Local Test"
read -p "Select BBS: " choice

case $choice in
    1) CONFIG="configs/telehack.conf" ;;
    2) CONFIG="configs/level29.conf" ;;
    3) CONFIG="configs/fozztexx.conf" ;;
    4) CONFIG="configs/local-test.conf" ;;
    *) echo "Invalid choice"; exit 1 ;;
esac

./build/modembridge -c $CONFIG -v
```

---

### Example 6: File Transfer with BBS

**Goal**: Download/upload files using ZMODEM protocol

**Configuration (file-transfer.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200              # High speed for file transfers
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=HARDWARE                # Enable flow control for reliability

TELNET_HOST=filebbs.com
TELNET_PORT=23

DATA_LOG_ENABLED=0           # Disable logging for file transfers
```

**Using with minicom:**
```bash
# Start ModemBridge
./build/modembridge -c file-transfer.conf

# Connect with minicom
minicom -D /dev/ttyUSB0 -b 115200

# In minicom:
# 1. Connect to BBS (ATA)
# 2. Navigate to file area
# 3. Start download from BBS
# 4. Press Ctrl-A Z
# 5. Select "rz" (Receive ZMODEM)
# 6. Files will download to current directory
```

**Note**: For file transfers, ensure:
- Hardware flow control enabled
- High baudrate (115200)
- No data logging (adds overhead)
- Stable connection

---

## BBS Host Scenarios

### Example 7: Simple BBS Host

**Goal**: Run BBS software that answers incoming "calls"

**Configuration (bbs-host.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

# Initialize modem (with H0 to reset)
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X4 S7=60 S10=7"

# Auto-answer after 2 rings (NO H0 here!)
MODEM_AUTOANSWER_COMMAND="ATS0=2"

# Local BBS telnet server
TELNET_HOST=localhost
TELNET_PORT=2323

# Enable logging for debugging
DATA_LOG_ENABLED=1
DATA_LOG_FILE=bbs-host.log
```

**BBS Software Setup (e.g., Synchronet):**

1. **Configure Synchronet to listen on telnet port 2323**
2. **Configure Synchronet modem settings:**
   - Device: /dev/ttyUSB0
   - Speed: 115200
   - Auto-answer: Enabled
   - DCD: Follows carrier (AT&C1)
   - DTR: Hangup on drop (AT&D2)

3. **Start ModemBridge:**
   ```bash
   ./build/modembridge -c bbs-host.conf -v
   ```

4. **Test connection:**
   ```bash
   # From another terminal
   telnet localhost 2323
   ```

   You should see:
   ```
   RING
   RING
   CONNECT 115200
   [BBS welcome screen]
   ```

---

### Example 8: Multi-Line BBS

**Goal**: Run multiple lines (nodes) simultaneously

**Setup:**

```bash
# Create 4 virtual serial port pairs
socat -d -d pty,raw,echo=0,link=/tmp/line1a pty,raw,echo=0,link=/tmp/line1b &
socat -d -d pty,raw,echo=0,link=/tmp/line2a pty,raw,echo=0,link=/tmp/line2b &
socat -d -d pty,raw,echo=0,link=/tmp/line3a pty,raw,echo=0,link=/tmp/line3b &
socat -d -d pty,raw,echo=0,link=/tmp/line4a pty,raw,echo=0,link=/tmp/line4b &
```

**Configuration Files:**

**line1.conf:**
```ini
SERIAL_PORT=/tmp/line1b
BAUDRATE=115200
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2"
MODEM_AUTOANSWER_COMMAND="ATS0=2"
TELNET_HOST=localhost
TELNET_PORT=2323
```

**line2.conf:** (TELNET_PORT=2324)
**line3.conf:** (TELNET_PORT=2325)
**line4.conf:** (TELNET_PORT=2326)

**Start All Lines:**
```bash
./build/modembridge -c line1.conf -d
./build/modembridge -c line2.conf -d
./build/modembridge -c line3.conf -d
./build/modembridge -c line4.conf -d
```

**BBS Software Configuration:**
- Configure 4 nodes on ports 2323-2326
- Each node monitors corresponding serial port

---

### Example 9: Door Game Server

**Goal**: Run external BBS door games via ModemBridge

**Configuration (door-server.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=HARDWARE                # Important for door games

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4"
MODEM_AUTOANSWER_COMMAND="ATS0=1"

# Door game server (e.g., DoorParty)
TELNET_HOST=door.example.com
TELNET_PORT=23

DATA_LOG_ENABLED=0
```

**Usage:**
- Door game expects modem on serial port
- ModemBridge bridges serial to door game server
- User connects via terminal program
- Can play classic door games (Trade Wars, LORD, etc.)

---

## Testing and Development

### Example 10: Virtual Serial Port Testing

**Goal**: Test ModemBridge without real hardware

**Setup:**
```bash
# Terminal 1: Create virtual port pair
socat -d -d pty,raw,echo=0,link=/tmp/vmodem0 \
             pty,raw,echo=0,link=/tmp/vmodem1

# Terminal 2: Configure and run ModemBridge
cat > virtual.conf <<EOF
SERIAL_PORT=/tmp/vmodem1
BAUDRATE=115200
TELNET_HOST=telehack.com
TELNET_PORT=23
EOF

./build/modembridge -c virtual.conf -v

# Terminal 3: Connect with minicom
minicom -D /tmp/vmodem0 -b 115200
```

**Advantages:**
- No hardware required
- Easy to automate tests
- Can create multiple virtual ports
- Great for development

---

### Example 11: Automated Testing

**Goal**: Automated test suite for AT commands

**Test Script (test-at-commands.sh):**
```bash
#!/bin/bash

SERIAL_PORT=/tmp/vmodem0
BAUDRATE=115200

# Function to send AT command and check response
send_at() {
    local cmd="$1"
    local expected="$2"

    echo "$cmd" > $SERIAL_PORT
    sleep 0.5
    local response=$(timeout 2 cat $SERIAL_PORT | head -1)

    if [[ "$response" == *"$expected"* ]]; then
        echo "✓ $cmd: OK"
        return 0
    else
        echo "✗ $cmd: FAILED (expected '$expected', got '$response')"
        return 1
    fi
}

echo "Starting AT command tests..."

# Basic commands
send_at "AT" "OK"
send_at "ATZ" "OK"
send_at "ATE1" "OK"
send_at "ATV1" "OK"
send_at "ATI" "ModemBridge"

# Extended commands
send_at "AT&C1" "OK"
send_at "AT&D2" "OK"
send_at "ATX4" "OK"

# S-registers
send_at "ATS0=2" "OK"
send_at "ATS0?" "002"

# Configuration
send_at "AT&V" "ACTIVE PROFILE"

echo "Tests complete!"
```

---

### Example 12: Performance Testing

**Goal**: Test data throughput and latency

**Configuration (performance.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=230400              # Highest speed
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=HARDWARE

TELNET_HOST=localhost
TELNET_PORT=9999

DATA_LOG_ENABLED=0           # Disable for accurate performance
```

**Test Server (test-server.py):**
```python
#!/usr/bin/env python3
import socket
import time

HOST = 'localhost'
PORT = 9999

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(1)
    print(f"Listening on {HOST}:{PORT}")

    conn, addr = s.accept()
    with conn:
        print(f"Connected by {addr}")

        # Send 1MB of data
        data = b'X' * (1024 * 1024)
        start = time.time()
        conn.sendall(data)
        elapsed = time.time() - start

        print(f"Sent {len(data)} bytes in {elapsed:.2f}s")
        print(f"Throughput: {len(data)/elapsed/1024:.2f} KB/s")
```

**Run Test:**
```bash
# Terminal 1: Start test server
python3 test-server.py

# Terminal 2: Start ModemBridge
./build/modembridge -c performance.conf

# Terminal 3: Connect and receive
minicom -D /dev/ttyUSB0 -b 230400 -C capture.txt
# Type ATA to connect
# Data will stream to capture.txt
```

---

## Advanced Configurations

### Example 13: High-Reliability Configuration

**Goal**: Maximum reliability for important connections

**Configuration (reliable.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=57600               # Lower speed = more reliable
BIT_PARITY=EVEN              # Add parity checking
BIT_DATA=8
BIT_STOP=2                   # Two stop bits for extra margin
FLOW=HARDWARE                # Hardware flow control

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4 S7=120 S10=14"

TELNET_HOST=critical-server.com
TELNET_PORT=23

DATA_LOG_ENABLED=1
DATA_LOG_FILE=reliable.log
```

**Settings Explained:**
- **Baudrate 57600**: Lower than maximum, reduces errors
- **Parity EVEN**: Detects single-bit errors
- **Stop bits 2**: Extra margin for timing
- **Hardware flow**: Prevents buffer overruns
- **S7=120**: Wait 2 minutes for carrier
- **S10=14**: Wait 1.4 seconds before disconnect on carrier loss

---

### Example 14: Low-Latency Gaming

**Goal**: Minimal latency for real-time BBS door games

**Configuration (low-latency.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=230400              # Highest speed
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=HARDWARE

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4 S10=3"

TELNET_HOST=gamebbs.com
TELNET_PORT=23

DATA_LOG_ENABLED=0           # No logging overhead
```

**Additional Optimization:**
```bash
# Run ModemBridge without verbose logging
./build/modembridge -c low-latency.conf

# Set real-time priority (requires root)
sudo chrt -f 99 ./build/modembridge -c low-latency.conf
```

---

### Example 15: Debugging Configuration

**Goal**: Maximum diagnostics for troubleshooting

**Configuration (debug.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4"
MODEM_AUTOANSWER_COMMAND="ATS0=0"

TELNET_HOST=testserver.com
TELNET_PORT=23

DATA_LOG_ENABLED=1
DATA_LOG_FILE=debug-session.log
```

**Run with All Debugging:**
```bash
# Terminal 1: Run ModemBridge with verbose logging
./build/modembridge -c debug.conf -v 2>&1 | tee modembridge-verbose.log

# Terminal 2: Monitor data log in real-time
tail -f debug-session.log | hexdump -C

# Terminal 3: Monitor serial port with strace
sudo strace -e read,write -s 1024 -p $(pidof modembridge)
```

---

## Scripted Automation

### Example 16: Automated BBS Tour

**Goal**: Connect to multiple BBSes automatically and log in

**Script (bbs-tour.sh):**
```bash
#!/bin/bash

# Configuration
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200

# BBS list with auto-login info
declare -A bbses=(
    ["Telehack"]="telehack.com:23:username:password"
    ["FozzTexx"]="bbs.fozztexx.com:23:guest:guest"
)

# Function to connect and send commands
visit_bbs() {
    local name="$1"
    local info="$2"
    IFS=':' read -r host port user pass <<< "$info"

    echo "=== Visiting $name ==="

    # Create temp config
    cat > /tmp/tour.conf <<EOF
SERIAL_PORT=$SERIAL_PORT
BAUDRATE=$BAUDRATE
TELNET_HOST=$host
TELNET_PORT=$port
EOF

    # Start ModemBridge in background
    ./build/modembridge -c /tmp/tour.conf &
    MODEM_PID=$!
    sleep 2

    # Send commands via serial port
    {
        echo "ATE1V1"
        sleep 1
        echo "ATA"
        sleep 5
        echo "$user"
        sleep 2
        echo "$pass"
        sleep 5
        echo "G"  # Goodbye command
        sleep 2
        echo "+++"
        sleep 2
        echo "ATH"
    } > $SERIAL_PORT

    # Stop ModemBridge
    kill $MODEM_PID

    echo "=== Left $name ==="
    echo
}

# Visit each BBS
for name in "${!bbses[@]}"; do
    visit_bbs "$name" "${bbses[$name]}"
done

echo "Tour complete!"
```

---

### Example 17: Scheduled BBS Mail Check

**Goal**: Automatically check BBS mail every hour

**Script (mail-check.sh):**
```bash
#!/bin/bash

LOG_FILE=/var/log/bbs-mail-check.log

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> $LOG_FILE
}

log "Starting mail check..."

# Start ModemBridge
./build/modembridge -c bbs-mail.conf -d
sleep 3

# Send AT commands to check mail
{
    echo "ATA"
    sleep 10
    echo "M"  # Mail command
    sleep 5
    echo "Q"  # Quit
    sleep 2
    echo "+++"
    sleep 2
    echo "ATH"
} > /dev/ttyUSB0 2>&1 | tee -a $LOG_FILE

log "Mail check complete."
```

**Cron Entry:**
```cron
# Check mail every hour
0 * * * * /home/user/mail-check.sh
```

---

## Integration Examples

### Example 18: Integration with Synchronet BBS

**Goal**: Use ModemBridge as modem for Synchronet BBS

**Synchronet Configuration (ctrl/sbbs.ini):**
```ini
[COM1]
Device=/dev/ttyUSB0
Baudrate=115200
DataBits=8
Parity=None
StopBits=1
FlowControl=None
```

**ModemBridge Configuration (synchronet.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE
BIT_DATA=8
BIT_STOP=1
FLOW=NONE

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4 S7=60"
MODEM_AUTOANSWER_COMMAND="ATS0=2"

TELNET_HOST=localhost
TELNET_PORT=23

DATA_LOG_ENABLED=1
DATA_LOG_FILE=synchronet-modem.log
```

**Startup Script:**
```bash
#!/bin/bash

# Start ModemBridge
./build/modembridge -c synchronet.conf -d

# Start Synchronet
cd /sbbs/exec
./sbbs
```

---

### Example 19: Integration with Mystic BBS

**Goal**: Use ModemBridge with Mystic BBS software

**Mystic Configuration:**
1. Configure telnet server on port 2323
2. Set up modem on /dev/ttyUSB0

**ModemBridge Configuration (mystic.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X4"
MODEM_AUTOANSWER_COMMAND="ATS0=2"

TELNET_HOST=localhost
TELNET_PORT=2323

DATA_LOG_ENABLED=0
```

---

### Example 20: DOS BBS via DOSBox

**Goal**: Run DOS BBS software via DOSBox with ModemBridge

**DOSBox Configuration (dosbox.conf):**
```ini
[serial]
serial1=directserial realport:ttyUSB0
```

**ModemBridge Configuration (dosbox-bbs.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=38400               # DOS-era speed

MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 X4"
MODEM_AUTOANSWER_COMMAND="ATS0=1"

TELNET_HOST=localhost
TELNET_PORT=2323

DATA_LOG_ENABLED=1
DATA_LOG_FILE=dosbox-bbs.log
```

**Steps:**
1. Start ModemBridge
2. Start DOSBox with serial passthrough
3. DOS BBS software sees "modem" on COM1
4. ModemBridge bridges to telnet server

---

## Troubleshooting Examples

### Example 21: Diagnosing Connection Failure

**Problem**: `ATA` returns `NO CARRIER`

**Diagnostic Configuration (diagnose.conf):**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=mystery-server.com
TELNET_PORT=23
DATA_LOG_ENABLED=1
DATA_LOG_FILE=diagnose.log
```

**Diagnostic Session:**
```bash
# Terminal 1: Run with verbose logging
./build/modembridge -c diagnose.conf -v 2>&1 | tee debug.log

# Terminal 2: Test telnet manually
telnet mystery-server.com 23

# Terminal 3: Monitor network traffic
sudo tcpdump -i any -A 'host mystery-server.com and port 23'

# Terminal 4: Connect with minicom
minicom -D /dev/ttyUSB0 -b 115200
```

**Analyze Results:**
```bash
# Check ModemBridge logs
grep -i "error\|fail\|connect" debug.log

# Check if telnet succeeded
# If telnet works but ATA doesn't, problem is ModemBridge
# If telnet fails, problem is network/server
```

---

### Example 22: Debugging Garbled Text

**Problem**: Text appears corrupted or garbled

**Test Configuration:**
```ini
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
TELNET_HOST=testserver.com
TELNET_PORT=23
DATA_LOG_ENABLED=1
DATA_LOG_FILE=garbled.log
```

**Diagnosis:**
```bash
# 1. Verify baudrate matches
./build/modembridge -c test.conf -v | grep -i baud

# 2. Check data log for corruption
hexdump -C garbled.log | less

# 3. Look for patterns
# - IAC sequences: FF FF (telnet escaping)
# - ANSI codes: 1B 5B (ESC [)
# - UTF-8 sequences: multi-byte characters

# 4. Test with different baudrates
for baud in 9600 19200 38400 57600 115200; do
    echo "Testing $baud..."
    sed -i "s/BAUDRATE=.*/BAUDRATE=$baud/" test.conf
    ./build/modembridge -c test.conf &
    sleep 2
    echo "AT" > /dev/ttyUSB0
    sleep 1
    kill %1
done
```

---

## Summary

These examples cover:
- **Basic setup and usage** for beginners
- **BBS client scenarios** for calling BBSes
- **BBS host scenarios** for running BBSes
- **Testing techniques** for development
- **Advanced configurations** for specific needs
- **Automation scripts** for repetitive tasks
- **Integration examples** with popular BBS software
- **Troubleshooting techniques** for problem solving

**Tips for Success:**
- Start with basic examples before advanced ones
- Always test with verbose logging first (`-v`)
- Enable data logging when troubleshooting
- Match baudrates between all components
- Check serial port permissions
- Test telnet connection manually first

---

**See Also:**
- [USER_GUIDE.md](USER_GUIDE.md) - Basic usage guide
- [AT_COMMANDS.md](AT_COMMANDS.md) - AT command reference
- [CONFIGURATION.md](CONFIGURATION.md) - Configuration options
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - Problem solving
