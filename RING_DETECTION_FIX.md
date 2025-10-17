# RING Detection Fix for SOFTWARE Mode (S0=0)

## Problem
When `MODEM_AUTOANSWER_MODE=0` (SOFTWARE mode), RING signals from the hardware modem were not being detected, preventing the software from sending the ATA command to answer incoming calls.

## Root Cause
RING messages arriving during the initialization buffer draining phase (approximately 2 seconds after startup) were being discarded without being checked. This is a critical window where early incoming calls could be lost.

## Solution
Modified `bridge.c` to check for RING messages even during the initialization buffer draining phase:

### Changes Made

1. **Buffer Draining Phase RING Detection** (bridge.c:800-828)
   - Added RING detection during the buffer drain loop
   - If RING is detected during draining, it's immediately processed through `modem_process_hardware_message()`
   - This prevents losing early incoming calls during the ~2 second initialization window

2. **Configuration Summary Display** (bridge.c:834-852)
   - Added detailed configuration summary after initialization
   - Shows current auto-answer mode (SOFTWARE/HARDWARE)
   - Displays S0 register value
   - Explains expected RING detection behavior
   - Helps verify configuration is correct before monitoring starts

## How RING Detection Works (SOFTWARE Mode)

### Configuration
```conf
MODEM_AUTOANSWER_MODE=0
MODEM_AUTOANSWER_SOFTWARE_COMMAND="ATE0 S0=0"
```

### Expected Behavior
1. **First RING arrives**
   - Hardware modem sends "RING\r\n"
   - `modem_process_hardware_message()` detects "RING"
   - Ring counter incremented: `SREG_RING_COUNT = 1`
   - Logs: `"*** RING detected from hardware modem ***"`
   - Logs: `"Ring count: 1, Auto-answer setting (S0): 0"`
   - Waits for second RING

2. **Second RING arrives** (typically 3-6 seconds after first)
   - Hardware modem sends "RING\r\n" again
   - Ring counter incremented: `SREG_RING_COUNT = 2`
   - Software detects: S0==0 && ring_count >= 2
   - Logs: `"=== SOFTWARE AUTO-ANSWER MODE (S0=0) ==="`
   - Logs: `"Ring threshold reached: 2/2 rings - sending ATA to hardware modem"`
   - **Sends ATA command**: `serial_write("ATA\r\n")`
   - Changes state to `MODEM_STATE_CONNECTING`
   - Logs: `"Modem state: CONNECTING (software-initiated)"`

3. **Modem answers call**
   - Hardware modem sends "CONNECT 2400" (or other speed)
   - `modem_process_hardware_message()` detects "CONNECT"
   - Changes state to `MODEM_STATE_ONLINE`
   - Connection established

## Verification Steps

### 1. Check Initialization Output
Look for this in the startup logs:
```
[INFO] === MODEM CONFIGURATION SUMMARY ===
[INFO]   Auto-answer mode: SOFTWARE (MODEM_AUTOANSWER_MODE=0)
[INFO]   S0 register value: 0
[INFO]   RING detection: Software will send ATA after 2 RINGs
[INFO] === Ready to monitor for RING signals ===
```

If S0 is not 0, check your `modembridge.conf` file.

### 2. Monitor RING Detection
When a call comes in, you should see:
```
[DEBUG] Hardware modem data (6 bytes): 52 49 4E 47 0D 0A
[DEBUG] Hardware modem ASCII: [RING]
[INFO] *** RING detected from hardware modem ***
[INFO] Ring count: 1, Auto-answer setting (S0): 0
```

Then after the second RING:
```
[INFO] === SOFTWARE AUTO-ANSWER MODE (S0=0) ===
[INFO] Ring threshold reached: 2/2 rings - sending ATA to hardware modem
[INFO] ATA command sent to hardware modem (5 bytes) - waiting for CONNECT response
[INFO] Modem state: CONNECTING (software-initiated)
```

### 3. Check Data Logging
If `DATA_LOG_ENABLED=1` in config, check `modembridge.log` for detailed hex dumps of all serial data, including RING messages.

## Common Issues

### RING Not Detected
**Symptoms**: No "RING detected" messages in logs, even though modem is ringing

**Possible causes**:
1. Serial port not receiving data - check cable and port configuration
2. RING message format different - some modems send "RING" with different line endings
3. Hardware modem S0 register set incorrectly - modem answering automatically instead of sending RING

**Debug steps**:
```bash
# Enable data logging in modembridge.conf
DATA_LOG_ENABLED=1

# Run modembridge in foreground
./build/modembridge -c modembridge.conf

# Check if ANY data is received from modem
tail -f modembridge.log

# Test modem directly with minicom
minicom -D /dev/ttyUSB0 -b 4800
# Type: ATS0?
# Should return: 000 (for SOFTWARE mode)
```

### RING Detected But No ATA Sent
**Symptoms**: See "RING detected" but no "sending ATA" message

**Possible causes**:
1. S0 register not set to 0 - check initialization logs
2. Ring count not reaching 2 - modem only sending 1 RING
3. Timing issue - RINGs too far apart (>20 seconds) causing buffer timeout

**Debug steps**:
- Check S0 value in initialization summary
- Count RING messages in logs - should see 2 before ATA
- Check timing between RINGs - should be 3-6 seconds

### ATA Sent But No CONNECT
**Symptoms**: See "ATA command sent" but no "CONNECT detected"

**Possible causes**:
1. Modem configuration issue - check S7 (wait time)
2. Line problem - no actual caller on other end
3. Modem speed mismatch

**Debug steps**:
- Check `modembridge.conf`: `S7=60` (60 second wait time)
- Test with real modem call, not just simulator
- Check modem response with minicom

## Code References

### RING Detection Logic
- `src/modem.c:883-948` - RING detection and ATA command sending
- `src/bridge.c:800-828` - Buffer draining with RING check
- `src/bridge.c:1689-1694` - Thread serial data processing

### Configuration
- `modembridge.conf:22` - MODEM_AUTOANSWER_MODE setting
- `modembridge.conf:28` - MODEM_AUTOANSWER_SOFTWARE_COMMAND
- `src/bridge.c:695-710` - Mode selection logic
- `src/bridge.c:768-779` - S0 register parsing and setting

## Testing Recommendations

1. **Test with actual hardware modem** - Simulators may not send RING correctly
2. **Use data logging** - Enable `DATA_LOG_ENABLED=1` to capture all serial data
3. **Run in foreground first** - Use `-v` flag for verbose output before daemonizing
4. **Check timing** - Verify RINGs arrive within 20 seconds (buffer timeout)
5. **Test early calls** - Try calling within 2 seconds of startup to verify drain phase fix

## References

Based on modem_sample MBSE BBS pattern:
- `../modem_sample/modem_sample.c:95-180` - wait_for_ring() implementation
- `../modem_sample/modem_control.c:272-275` - detect_ring() function
- `../modem_sample/modem_control.c:315-389` - modem_answer_with_speed_adjust()
