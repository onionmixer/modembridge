# AT Commands Reference

Complete reference for all AT commands supported by ModemBridge.

## Table of Contents

1. [Introduction](#introduction)
2. [Basic Commands](#basic-commands)
3. [Extended Commands](#extended-commands)
4. [S-Registers](#s-registers)
5. [Result Codes](#result-codes)
6. [Command Chaining](#command-chaining)
7. [Examples](#examples)

## Introduction

ModemBridge implements a Hayes-compatible AT command set. Commands are case-insensitive (AT, at, At all work) and can be chained together on a single line.

### Command Format

```
AT[command][parameters]<CR>
```

- `AT`: Attention prefix (required for most commands)
- `[command]`: One or more command characters
- `[parameters]`: Optional numeric or string parameters
- `<CR>`: Carriage return (Enter key)

### Response Format

In **verbose mode** (default):
```
<CR><LF>response<CR><LF>
```

In **numeric mode**:
```
<CR><LF>code<CR><LF>
```

## Basic Commands

### AT - Attention / Test

**Syntax**: `AT`

**Description**: Tests if modem is responding. Use this to verify communication.

**Response**: `OK`

**Example**:
```
AT
OK
```

---

### ATA - Answer

**Syntax**: `ATA`

**Description**: Answers an incoming call or initiates connection to the configured telnet server.

**Response**:
- `OK` - Command accepted
- `CONNECT [baudrate]` - Connection established
- `NO CARRIER` - Connection failed

**Example**:
```
ATA
OK
CONNECT 115200
```

---

### ATD - Dial

**Syntax**: `ATD[dial_string]`

**Description**: Dial command (placeholder). ModemBridge connects via ATA, but ATD is accepted for compatibility.

**Parameters**:
- `dial_string`: Any string (ignored)

**Response**: `OK`

**Example**:
```
ATDT5551234
OK
```

---

### ATE - Echo

**Syntax**: `ATE[n]`

**Description**: Controls command echo (whether typed characters are echoed back).

**Parameters**:
- `0`: Echo off
- `1`: Echo on (default)

**Response**: `OK`

**Example**:
```
ATE0
OK
(now typing won't be echoed)

ATE1
OK
(echo restored)
```

---

### ATH - Hang Up

**Syntax**: `ATH[n]`

**Description**: Terminates the current connection and returns to command mode.

**Parameters**:
- `0` or omitted: Go on-hook (hang up)

**Response**:
- `OK`
- `NO CARRIER` (after hanging up)

**Example**:
```
ATH
OK
NO CARRIER
```

---

### ATI - Information

**Syntax**: `ATI[n]`

**Description**: Displays product information.

**Parameters**:
- `0` or omitted: Product identification

**Response**: ModemBridge version and `OK`

**Example**:
```
ATI
ModemBridge v1.0.0
OK
```

---

### ATO - Return to Online Mode

**Syntax**: `ATO[n]`

**Description**: Returns to online (data) mode after escape sequence (+++).

**Parameters**:
- `0` or omitted: Go online

**Response**:
- `CONNECT` - If carrier present
- `NO CARRIER` - If no connection

**Example**:
```
(in command mode after +++)
ATO
CONNECT
(back to data mode)
```

---

### ATQ - Quiet Mode

**Syntax**: `ATQ[n]`

**Description**: Controls whether result codes are sent.

**Parameters**:
- `0`: Result codes enabled (default)
- `1`: Result codes disabled (quiet mode)

**Response**: `OK` (if Q0, nothing if Q1)

**Example**:
```
ATQ1
(no response - quiet mode on)

ATQ0
OK
(responses restored)
```

---

### ATV - Verbose Mode

**Syntax**: `ATV[n]`

**Description**: Controls response format.

**Parameters**:
- `0`: Numeric responses (0, 1, 2, 3, 4...)
- `1`: Verbose responses (OK, CONNECT, ERROR...) (default)

**Response**: `OK` or `0`

**Example**:
```
ATV0
0
(numeric mode)

ATV1
OK
(verbose mode)
```

---

### ATZ - Reset

**Syntax**: `ATZ[n]`

**Description**: Resets modem to default configuration.

**Parameters**:
- `0` or omitted: Reset to profile 0

**Response**: `OK`

**Example**:
```
ATZ
OK
```

---

## Extended Commands

### AT&C - DCD Control

**Syntax**: `AT&C[n]`

**Description**: Controls Data Carrier Detect (DCD) signal behavior.

**Parameters**:
- `0`: DCD always ON
- `1`: DCD follows carrier state (default, recommended)

**Response**: `OK`

**Notes**:
- With &C1, DCD reflects actual connection state
- Helps BBS software detect disconnections

**Example**:
```
AT&C1
OK
```

---

### AT&D - DTR Control

**Syntax**: `AT&D[n]`

**Description**: Controls how modem responds to DTR (Data Terminal Ready) signal.

**Parameters**:
- `0`: DTR ignored
- `1`: DTR OFF → command mode
- `2`: DTR OFF → hang up and command mode (default, recommended)
- `3`: DTR OFF → reset modem

**Response**: `OK`

**Notes**:
- &D2 is standard for BBS operation
- Allows clean disconnection when closing serial port

**Example**:
```
AT&D2
OK
```

---

### AT&F - Factory Defaults

**Syntax**: `AT&F`

**Description**: Resets all settings to factory defaults.

**Response**: `OK`

**Example**:
```
AT&F
OK
```

---

### AT&S - DSR Override

**Syntax**: `AT&S[n]`

**Description**: Controls Data Set Ready (DSR) signal.

**Parameters**:
- `0`: DSR always on (default)
- `1`: DSR tracks modem state

**Response**: `OK`

**Example**:
```
AT&S0
OK
```

---

### AT&V - View Configuration

**Syntax**: `AT&V`

**Description**: Displays current modem configuration.

**Response**: Multi-line configuration display, then `OK`

**Example**:
```
AT&V
ACTIVE PROFILE:
E1 Q0 V1 X4
&C1 &D2 &S0
B0 L2 M1 \N3

S-REGISTERS:
S00:002 S01:000 S02:043 S03:013
S04:010 S05:008 S06:002 S07:060
S08:002 S09:006 S10:007 S11:095
S12:050 S13:000 S14:000 S15:000

OK
```

---

### AT&W - Write Configuration

**Syntax**: `AT&W[n]`

**Description**: Saves current configuration to profile memory.

**Parameters**:
- `0`: Save to profile 0 (default)
- `1`: Save to profile 1

**Response**: `OK`

**Notes**: Configuration is saved in memory (not persistent across restarts)

**Example**:
```
AT&W0
OK
```

---

### ATB - Communication Standard

**Syntax**: `ATB[n]`

**Description**: Selects communication standard.

**Parameters**:
- `0`: CCITT (ITU-T) standard (default)
- `1`: Bell standard

**Response**: `OK`

**Example**:
```
ATB0
OK
```

---

### ATL - Speaker Volume

**Syntax**: `ATL[n]`

**Description**: Sets speaker volume (cosmetic, no actual speaker).

**Parameters**:
- `0`: Low
- `1`: Low
- `2`: Medium (default)
- `3`: High

**Response**: `OK`

**Example**:
```
ATL2
OK
```

---

### ATM - Speaker Control

**Syntax**: `ATM[n]`

**Description**: Controls when speaker is on (cosmetic).

**Parameters**:
- `0`: Speaker always off
- `1`: Speaker on until carrier detected (default)
- `2`: Speaker always on
- `3`: Speaker on after dial, off when carrier detected

**Response**: `OK`

**Example**:
```
ATM1
OK
```

---

### ATX - Extended Result Codes

**Syntax**: `ATX[n]`

**Description**: Controls which result codes are sent.

**Parameters**:
- `0`: Basic codes only (OK, CONNECT, RING, NO CARRIER, ERROR)
- `1`: X0 + connection speed
- `2`: X1 + NO DIALTONE
- `3`: X1 + BUSY
- `4`: All extended codes (default, recommended)

**Response**: `OK`

**Example**:
```
ATX4
OK
```

**Result Code Table**:

| Code | X0 | X1 | X2 | X3 | X4 | Meaning |
|------|----|----|----|----|----| --------|
| OK | ✓ | ✓ | ✓ | ✓ | ✓ | Command successful |
| CONNECT | ✓ | ✓ | ✓ | ✓ | ✓ | Connection established |
| RING | ✓ | ✓ | ✓ | ✓ | ✓ | Incoming call |
| NO CARRIER | ✓ | ✓ | ✓ | ✓ | ✓ | No connection |
| ERROR | ✓ | ✓ | ✓ | ✓ | ✓ | Command error |
| CONNECT [speed] | - | ✓ | ✓ | ✓ | ✓ | Connection with speed |
| NO DIALTONE | - | - | ✓ | - | ✓ | No dial tone |
| BUSY | - | - | - | ✓ | ✓ | Line busy |
| NO ANSWER | - | - | - | - | ✓ | No answer |

---

### AT\N - Error Correction Mode

**Syntax**: `AT\N[n]`

**Description**: Selects error correction mode.

**Parameters**:
- `0`: Normal mode (no error correction)
- `1`: Direct mode
- `2`: Reliable mode (MNP)
- `3`: Auto reliable mode (default)

**Response**: `OK`

**Example**:
```
AT\N3
OK
```

---

## S-Registers

S-registers store modem parameters. Access with `ATS` commands.

### Reading S-Registers

**Syntax**: `ATS[n]?`

**Example**:
```
ATS0?
002
OK
```

### Writing S-Registers

**Syntax**: `ATS[n]=[value]`

**Example**:
```
ATS0=2
OK
```

### S-Register Reference

| Register | Default | Range | Description |
|----------|---------|-------|-------------|
| S0 | 0 | 0-255 | Auto-answer ring count (0=disabled) |
| S1 | 0 | 0-255 | Ring counter (read-only) |
| S2 | 43 | 0-255 | Escape character ('+' = 43) |
| S3 | 13 | 0-127 | Carriage return character |
| S4 | 10 | 0-127 | Line feed character |
| S5 | 8 | 0-127 | Backspace character |
| S6 | 2 | 2-255 | Wait time before dial (seconds) |
| S7 | 60 | 1-255 | Wait for carrier (seconds) |
| S8 | 2 | 0-255 | Pause time for comma in dial string |
| S9 | 6 | 1-255 | Carrier detect response time (0.1s) |
| S10 | 7 | 1-255 | Carrier loss disconnect time (0.1s) |
| S11 | 95 | 50-255 | DTMF tone duration (milliseconds) |
| S12 | 50 | 0-255 | Escape guard time (0.02s units) |

### Important S-Registers for BBS Operation

**S0 - Auto-Answer**:
```
ATS0=0    # Disable auto-answer
ATS0=1    # Answer after 1 ring
ATS0=2    # Answer after 2 rings (recommended)
```

**S2 - Escape Character**:
```
ATS2=43   # Use '+' as escape (default)
ATS2=126  # Use '~' as escape
```

**S7 - Carrier Wait Time**:
```
ATS7=60   # Wait 60 seconds for carrier (default)
ATS7=30   # Wait 30 seconds (faster timeout)
```

**S10 - Carrier Loss Delay**:
```
ATS10=7   # Wait 0.7 seconds after carrier loss (default)
ATS10=14  # Wait 1.4 seconds (more tolerant)
```

## Result Codes

### Verbose Mode (ATV1)

| Code | Meaning | When Sent |
|------|---------|-----------|
| OK | Success | After command execution |
| CONNECT | Connected | After successful ATA/ATO |
| CONNECT [speed] | Connected with speed | With ATX1 or higher |
| RING | Incoming call | When auto-answer enabled |
| NO CARRIER | Disconnected | After connection loss |
| ERROR | Command error | Invalid command/parameter |
| NO DIALTONE | No dial tone | With ATX2 or ATX4 |
| BUSY | Line busy | With ATX3 or ATX4 |
| NO ANSWER | No answer | With ATX4 |

### Numeric Mode (ATV0)

| Code | Verbose Equivalent |
|------|--------------------|
| 0 | OK |
| 1 | CONNECT |
| 2 | RING |
| 3 | NO CARRIER |
| 4 | ERROR |
| 5 | CONNECT 1200 |
| 6 | NO DIALTONE |
| 7 | BUSY |
| 8 | NO ANSWER |

## Command Chaining

Multiple commands can be sent on one line:

```
ATE1V1Q0X4
OK
```

This executes:
- `ATE1` - Echo on
- `ATV1` - Verbose on
- `ATQ0` - Quiet off
- `ATX4` - Extended codes

**Rules**:
- All commands must start with `AT`
- Commands are executed left to right
- If any command fails, remaining commands are not executed
- One response is sent at the end

**Examples**:
```
AT&C1&D2B0
OK

ATE1V1S0=2
OK

AT&F&C1&D2X4S0=2
OK
```

## Examples

### Basic Setup

```
# Test connection
AT
OK

# Configure for BBS use
ATE1V1Q0X4&C1&D2
OK

# Enable auto-answer (2 rings)
ATS0=2
OK

# View configuration
AT&V
(configuration display)
OK
```

### Connecting to a Server

```
# Enable echo and verbose mode
ATE1V1
OK

# Connect
ATA
OK
CONNECT 115200
(now in data mode - connected to server)
```

### Escape and Disconnect

```
(in data mode)

(wait 1 second)
+++
(wait 1 second)

OK

ATH
OK
NO CARRIER
```

### Resume After Escape

```
(after +++ escape)

OK

ATO
CONNECT
(back in data mode)
```

### Configuration for BBS Host

```
# Reset to defaults
ATZ
OK

# Configure DCD and DTR
AT&C1&D2
OK

# Extended result codes
ATX4
OK

# Auto-answer after 2 rings
ATS0=2
OK

# Set carrier wait to 60 seconds
ATS7=60
OK

# Save configuration
AT&W0
OK

# Verify
AT&V
(shows all settings)
OK
```

## Quick Reference Card

### Most Used Commands

| Command | Description |
|---------|-------------|
| `AT` | Test |
| `ATE1` | Echo on |
| `ATV1` | Verbose on |
| `ATA` | Connect |
| `+++` | Escape to command mode |
| `ATO` | Return online |
| `ATH` | Hang up |
| `AT&V` | View config |
| `ATZ` | Reset |

### Recommended Settings

```
ATE1V1Q0X4&C1&D2S0=2S7=60S10=7
```

This sets:
- Echo ON
- Verbose ON
- Quiet OFF
- Extended codes (all)
- DCD follows carrier
- DTR hangup
- Auto-answer (2 rings)
- Carrier wait (60s)
- Carrier loss delay (0.7s)

---

**See Also**:
- [USER_GUIDE.md](USER_GUIDE.md) - General usage
- [CONFIGURATION.md](CONFIGURATION.md) - Configuration options
- [EXAMPLES.md](EXAMPLES.md) - Usage scenarios
