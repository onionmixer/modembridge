# ModemBridge

A high-performance dialup modem emulator that bridges serial port connections to telnet servers, enabling vintage computing enthusiasts and BBS operators to connect classic systems to modern network services.

## Overview

ModemBridge emulates a Hayes-compatible dialup modem, translating AT commands from serial-connected clients into telnet connections. This allows retrocomputing systems, terminal programs, and BBS software to communicate with telnet-based services as if they were connecting through a traditional telephone modem.

## Documentation

Comprehensive documentation is available in the [docs/](docs/) directory:

- **[User Guide](docs/USER_GUIDE.md)** - Complete guide for installing, configuring, and using ModemBridge
- **[AT Commands Reference](docs/AT_COMMANDS.md)** - Detailed reference for all supported AT commands
- **[Configuration Guide](docs/CONFIGURATION.md)** - Complete configuration file reference and examples
- **[Troubleshooting Guide](docs/TROUBLESHOOTING.md)** - Solutions to common problems and debugging techniques
- **[Examples](docs/EXAMPLES.md)** - Real-world usage examples and configuration scenarios
- **[API Reference](docs/API_REFERENCE.md)** - Developer documentation for the codebase

**Quick Start**: See the [User Guide](docs/USER_GUIDE.md) to get started in 5 minutes.

## Features

### 🆕 Level 3 Advanced Pipeline Management
- **Sophisticated State Machine**: 10-state system with timeout handling and DCD-based activation
- **Quantum-based Scheduling**: Fair round-robin with anti-starvation algorithms (50ms base quantum)
- **Watermark-based Buffer Management**: Dynamic sizing with 5-level overflow protection
- **Performance Monitoring**: Real-time latency tracking and comprehensive metrics
- **Anti-starvation Algorithms**: 500ms threshold prevents pipeline starvation
- **Dynamic Buffer Sizing**: Adaptive growth/shrink based on usage patterns
- **Memory Pool Management**: Fragmentation prevention with fixed-size block allocation
- **Real-time Chat Server Optimization**: Sub-100ms latency for interactive applications

### Modem Emulation
- **Hayes AT Command Set**: Full support for essential AT commands (ATA, ATD, ATH, ATZ, ATE, ATV, ATQ, ATS, ATO, AT&F)
- **Extended AT Commands**: AT&C, AT&D, ATB, ATX, ATL, ATM, AT\N, AT&S, AT&V, AT&W (Phase 16)
- **Escape Sequence Detection**: Implements the `+++` escape sequence with proper guard time (1 second)
- **S-Register Support**: Configurable modem parameters through S-registers (S0-S15)
- **Online/Command Mode**: Proper state management between command and data modes
- **DCD/DTR Management**: Enhanced control with edge detection and state transitions (Phase 15)
- **Auto-Answer Support**: S0 register for automatic connection answering (Phase 15)
- **Flow Control**: Support for NONE, XON/XOFF, RTS/CTS, and BOTH modes

### Protocol Handling
- **Telnet Protocol**: RFC 854 compliant with IAC (Interpret As Command) negotiation
- **ANSI Filtering**: Removes ANSI escape sequences from modem output to telnet
- **UTF-8 Support**: Proper handling of multibyte character boundaries
- **Binary Mode**: Supports 8-bit clean data transfer

### Performance
- **Non-blocking I/O**: Uses select() for efficient multiplexing
- **Zero Dependencies**: Pure C implementation using only glibc and POSIX APIs
- **Ultra-low Latency**: Sub-100ms average with Level 3 quantum scheduling
- **Dynamic Baudrate**: Runtime baudrate negotiation from 300 to 230400 bps
- **Adaptive Performance**: Level 3 automatically optimizes for workload (chat servers, file transfers, etc.)
- **1200 bps Optimization**: Special handling for low-speed modem connections
- **Memory Efficiency**: Dynamic allocation with <1% overflow rate target

### Operational Features
- **Daemon Mode**: Background operation with syslog integration
- **Signal Handling**: Graceful shutdown (SIGTERM, SIGINT) and config reload (SIGHUP)
- **Statistics Tracking**: Connection duration and byte transfer counters
- **Comprehensive Logging**: Multiple log levels (DEBUG, INFO, WARNING, ERROR)

## Requirements

### System Requirements
- **OS**: Linux (Ubuntu 22.04 LTS or newer recommended)
- **Kernel**: 2.6+ with POSIX support
- **Architecture**: x86_64, ARM, or any POSIX-compliant system

### Build Requirements
- **Compiler**: GCC 4.8+ or Clang 3.4+
- **Make**: GNU Make 3.81+
- **Standard C Library**: glibc 2.17+ or musl

### Runtime Requirements
- **Serial Port**: USB-to-Serial adapter (FTDI, Prolific, etc.) or native serial port
- **Permissions**: Read/write access to serial device (typically `/dev/ttyUSB*` or `/dev/ttyACM*`)
- **Network**: TCP/IP connectivity to target telnet servers

## Installation

### Building from Source

ModemBridge supports multiple build levels for different use cases. Each level enables specific features and optimizations.

#### Build Levels Overview

- **Level 1**: Serial modem emulation only (no telnet support)
- **Level 2**: Serial + Telnet bridging (default for most users)
- **Level 3**: Advanced pipeline management with performance optimizations

#### Build Commands

```bash
# Clone the repository
git clone https://github.com/yourusername/modembridge.git
cd modembridge

# Build Level 3 (default) - Full feature set with advanced pipeline management
make
# or
make level3

# Build Level 2 - Serial + Telnet bridging (recommended for most users)
make level2

# Build Level 1 - Serial modem emulation only
make level1

# Clean previous build artifacts
make clean

# Show current build configuration
make config

# Show Makefile variables (for debugging)
make show

# The executable will be at: build/modembridge
```

#### Build Level Details

**Level 1 (Serial Only)**
- Serial port communication and Hayes AT command emulation
- Direct modem-to-client communication without telnet bridging
- Minimal dependencies and resource usage
- Ideal for standalone modem testing and development
- Build flags: `ENABLE_LEVEL1`

**Level 2 (Serial + Telnet)**
- Full Level 1 features plus telnet protocol support
- Bidirectional bridging between serial and telnet connections
- ANSI filtering and UTF-8 multibyte character handling
- Recommended for most retrocomputing and BBS use cases
- Build flags: `ENABLE_LEVEL1`, `ENABLE_LEVEL2`

**Level 3 (Advanced Pipeline)**
- Complete Level 2 features plus advanced pipeline management
- Quantum-based scheduling and watermark-based buffer management
- Performance optimizations for real-time applications
- Sub-100ms latency with anti-starvation algorithms
- Dynamic buffer sizing and memory pool management
- Build flags: `ENABLE_LEVEL1`, `ENABLE_LEVEL2`, `ENABLE_LEVEL3`

#### Complete Makefile Reference

The Makefile provides comprehensive build and development targets:

```bash
# Build Targets
make                    # Build Level 3 (default)
make level1           # Build Level 1 (serial only)
make level2           # Build Level 2 (serial + telnet)
make level3           # Build Level 3 (full feature set)

# Build Management
make clean            # Remove all build artifacts
make debug           # Build with debug symbols and no optimization
make config          # Show current build configuration
make show            # Show Makefile variables (for debugging)

# Installation
make install          # Install to system (requires root)
make uninstall        # Remove from system (requires root)

# Testing
make run              # Build and run with default config
make help             # Show available targets and usage
```

#### Build Options

You can customize the build with environment variables:

```bash
# Debug build with symbols
make DEBUG=1

# Custom build mode
make BUILD_MODE=level2

# Custom installation prefix
make install PREFIX=/opt/modembridge
```

#### Build Flags Reference

Each build level uses specific preprocessor flags:

- **Level 1**: `-DENABLE_LEVEL1`
- **Level 2**: `-DENABLE_LEVEL1 -DENABLE_LEVEL2`
- **Level 3**: `-DENABLE_LEVEL1 -DENABLE_LEVEL2 -DENABLE_LEVEL3`

These flags enable conditional compilation of features specific to each level, ensuring optimal binary size and performance for the intended use case.

### Installation

```bash
# Install to /usr/local/bin (requires root)
sudo make install

# Or specify custom installation prefix
sudo make install PREFIX=/opt/modembridge
```

### Uninstallation

```bash
sudo make uninstall
```

## Configuration

### Configuration File

Create a configuration file (default: `/etc/modembridge.conf`):

```ini
# Serial Port Configuration
SERIAL_PORT=/dev/ttyUSB0
BAUDRATE=115200
BIT_PARITY=NONE       # NONE, EVEN, ODD
BIT_DATA=8            # 5, 6, 7, or 8
BIT_STOP=1            # 1 or 2
FLOW=NONE             # NONE, SOFTWARE, HARDWARE, BOTH

# Modem Initialization (Phase 13, executed during health check)
MODEM_INIT_COMMAND="ATH0; AT&C1 &D2 B0 X4 S7=60 S10=7"

# Modem Auto-Answer (Phase 15, executed after server start, NO H0!)
MODEM_AUTOANSWER_COMMAND="ATS0=2"

# Telnet Server Configuration
TELNET_HOST=bbs.example.com
TELNET_PORT=23

# Data Logging (optional)
DATA_LOG_ENABLED=1
DATA_LOG_FILE=modembridge.log
```

See the [Configuration Guide](docs/CONFIGURATION.md) for complete details on all configuration options.

### Serial Port Permissions

Add your user to the dialout group to access serial ports:

```bash
sudo usermod -a -G dialout $USER
# Log out and back in for changes to take effect
```

## Usage

### Basic Usage

The appropriate ModemBridge binary should be used based on your requirements:

```bash
# Run in foreground with default config
modembridge

# Run with custom config file
modembridge -c /path/to/config.conf

# Run in daemon mode
modembridge -d

# Enable verbose logging
modembridge -v
```

### Choosing the Right Build Level

**For most retrocomputing and BBS users:**
```bash
make level2    # Recommended for telnet bridging
```

**For modem development and testing:**
```bash
make level1    # Serial emulation only
```

**For high-performance real-time applications:**
```bash
make level3    # Advanced pipeline management
```

### Command Line Options

```
Usage: modembridge [options]

Options:
  -c, --config FILE    Configuration file (default: /etc/modembridge.conf)
  -d, --daemon         Run as daemon
  -p, --pid-file FILE  PID file (default: /var/run/modembridge.pid)
  -v, --verbose        Verbose logging (DEBUG level)
  -h, --help           Show help message
  -V, --version        Show version information
```

### Supported AT Commands

ModemBridge supports a comprehensive set of Hayes-compatible AT commands:

**Basic Commands**: AT, ATA, ATD, ATE, ATH, ATI, ATO, ATQ, ATV, ATZ

**Extended Commands**: AT&C, AT&D, AT&F, AT&S, AT&V, AT&W, ATB, ATL, ATM, ATX, AT\N

**S-Registers**: S0-S15 (auto-answer, escape character, timeouts, etc.)

See the [AT Commands Reference](docs/AT_COMMANDS.md) for complete documentation with examples and parameter details.

### Escape Sequence

To return to command mode from online (data) mode:

1. Wait 1 second (guard time)
2. Type `+++` (three plus signs)
3. Wait 1 second (guard time)

The modem will respond with `OK` and enter command mode while maintaining the carrier.

### Connection Flow

```
Terminal → Serial → ModemBridge → Telnet → BBS/Server

1. Terminal sends: ATA
2. ModemBridge responds: OK
3. ModemBridge connects to telnet server
4. ModemBridge responds: CONNECT 115200
5. Data flows bidirectionally
6. Terminal sends: +++ (escape)
7. ModemBridge responds: OK (command mode)
8. Terminal sends: ATH
9. ModemBridge responds: OK
10. ModemBridge sends: NO CARRIER
```

## Testing

### 🆕 Level 3 Advanced Testing

The `tests/` directory contains comprehensive Level 3 testing utilities:

**Level 3 Test Suite** (`test_level3.sh`)
- Comprehensive testing of all Level 3 features
- State machine transition validation
- Quantum scheduling verification
- Buffer management stress testing
- Performance benchmarking
- Integration with Level 1/2 components

```bash
cd tests
./test_level3.sh  # Run all Level 3 tests
```

**Level 3 Benchmarking** (`benchmark_level3.sh`)
- Performance validation against LEVEL3_WORK_TODO.txt targets
- Latency measurement and tracking
- Buffer overflow rate testing
- 1200 bps optimization verification
- Comprehensive performance reporting

```bash
./benchmark_level3.sh  # Run performance benchmarks
```

**Implementation Validation** (`validate_level3.sh`)
- Code structure verification
- Feature completeness checking
- Compilation validation
- Documentation review

```bash
./validate_level3.sh  # Validate implementation
```

### Level 2 Telnet Testing

The `tests/` directory also contains specialized Level 2 telnet testing utilities:

**Standalone Telnet Tester** (`telnet_test_standalone.c`)
- Independent telnet client that doesn't require modem hardware
- Tests multi-language text transmission ("abcd", "한글", "こんにちは。")
- Sends test strings at 3-second intervals
- Verifies echo responses from telnet servers
- Supports all three telnet server modes (line, character, binary)

```bash
# Build the standalone tester
gcc -Wall -Wextra -O2 -std=gnu11 -D_GNU_SOURCE -DENABLE_LEVEL2 -Iinclude -o telnet_test_standalone tests/telnet_test_standalone.c src/telnet.c src/common.c -lpthread

# Test against different telnet servers
./telnet_test_standalone -h 127.0.0.1 -p 9091 -d 30 -v  # Line mode server
./telnet_test_standalone -h 127.0.0.1 -p 9092 -d 30 -v  # Character mode server
./telnet_test_standalone -h 127.0.0.1 -p 9093 -d 30 -v  # Binary mode server
```

**Automated Test Script** (`test_telnet.sh`)
- Runs comprehensive tests across multiple telnet servers
- Generates detailed logs for analysis
- Tests both text reception and transmission capabilities

```bash
cd tests
./test_telnet.sh
```

### Testing with socat

Create virtual serial port pairs for testing without hardware:

```bash
# Terminal 1: Create virtual serial ports
socat -d -d pty,raw,echo=0,link=/tmp/vmodem0 pty,raw,echo=0,link=/tmp/vmodem1

# Terminal 2: Run modembridge
modembridge -c modembridge.conf -v

# Terminal 3: Connect with minicom or screen
minicom -D /tmp/vmodem0
# or
screen /tmp/vmodem0 115200

# Test AT commands:
AT          # Should respond: OK
ATE1        # Enable echo
ATA         # Should respond: OK, then CONNECT 115200
+++         # Wait 1 second before and after
ATH         # Should respond: OK, then NO CARRIER
```

### Testing with Real Hardware

1. **Connect USB-to-Serial adapter**
   ```bash
   # Check device name
   dmesg | grep tty
   # Usually shows: /dev/ttyUSB0 or /dev/ttyACM0
   ```

2. **Configure modembridge**
   ```bash
   # Edit modembridge.conf
   SERIAL_PORT=/dev/ttyUSB0
   BAUDRATE=115200
   TELNET_HOST=telehack.com
   TELNET_PORT=23
   ```

3. **Run modembridge**
   ```bash
   modembridge -c modembridge.conf -v
   ```

4. **Connect with terminal program**
   - Windows: Use PuTTY, TeraTerm, or HyperTerminal
   - macOS: Use screen or minicom
   - Linux: Use minicom, screen, or cu

5. **Test connection**
   ```
   AT          → OK
   ATA         → OK, CONNECT 115200
   (connected to telnet server)
   +++         → OK (back to command mode)
   ATO         → CONNECT (back online)
   +++         → OK
   ATH         → OK, NO CARRIER (disconnected)
   ```

## Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────────┐
│                     ModemBridge                          │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────┐    ┌────────┐    ┌────────┐    ┌────────┐│
│  │ Serial   │───▶│ Modem  │───▶│ Bridge │───▶│ Telnet ││
│  │ Port     │◀───│ Emu    │◀───│ Engine │◀───│ Client ││
│  └──────────┘    └────────┘    └────────┘    └────────┘│
│       │              │              │             │      │
│       │              │              │             │      │
│  /dev/ttyUSB0   AT Commands    select()    TCP Socket  │
│   termios API    Hayes Mode   Non-blocking   RFC 854   │
└─────────────────────────────────────────────────────────┘
```

### Data Flow

**Serial → Telnet (Modem to Server)**
```
Serial Port → Raw Bytes → Modem Layer (escape detection) →
ANSI Filter (remove CSI) → Telnet Encoder (IAC escape) →
TCP Socket → Telnet Server
```

**Telnet → Serial (Server to Modem)**
```
Telnet Server → TCP Socket → Telnet Decoder (IAC processing) →
ANSI Passthrough → Raw Bytes → Serial Port
```

### Key Modules

- **serial.c**: Serial port I/O using termios API
- **modem.c**: Hayes AT command processor and state machine
- **telnet.c**: Telnet protocol handler (RFC 854)
- **bridge.c**: Main I/O multiplexing and data routing
- **config.c**: Configuration file parser
- **common.c**: Utility functions and helpers

## Troubleshooting

### Quick Fixes

**Serial port permission denied**
```bash
sudo usermod -a -G dialout $USER
# Log out and back in
```

**No response to AT commands**
```bash
# Enable verbose logging to see what's happening
modembridge -v -c modembridge.conf
```

**Connection fails**
```bash
# Test telnet manually first
telnet bbs.example.com 23
```

### Complete Troubleshooting Guide

For detailed troubleshooting, error messages, debugging techniques, and FAQ, see the [Troubleshooting Guide](docs/TROUBLESHOOTING.md).

## Project Structure

```
modembridge/
├── include/              # Header files
│   ├── bridge.h         # Bridge engine definitions
│   ├── common.h         # Common macros and types
│   ├── config.h         # Configuration structures
│   ├── modem.h          # Modem emulation
│   ├── serial.h         # Serial port interface
│   └── telnet.h         # Telnet protocol
├── src/                 # Source files
│   ├── bridge.c         # Main bridge logic
│   ├── common.c         # Utility functions
│   ├── config.c         # Config parser
│   ├── main.c           # Entry point
│   ├── modem.c          # AT command processor
│   ├── serial.c         # Serial I/O
│   └── telnet.c         # Telnet client
├── docs/                # Documentation
│   ├── USER_GUIDE.md    # User guide
│   ├── AT_COMMANDS.md   # AT command reference
│   ├── CONFIGURATION.md # Configuration guide
│   ├── TROUBLESHOOTING.md # Troubleshooting guide
│   ├── EXAMPLES.md      # Usage examples
│   ├── API_REFERENCE.md # Developer API docs
│   └── LEVEL3_INTEGRATION_GUIDE.md # Level 3 integration guide
├── build/               # Build artifacts (generated)
│   ├── obj/             # Object files
│   └── modembridge      # Final executable
├── Makefile             # Build system
├── modembridge.conf     # Example configuration
├── tests/               # Test files and utilities
│   ├── telnet_test_standalone.c    # Standalone Level 2 telnet tester
│   ├── telnet_test.h               # Telnet test module header
│   ├── telnet_test.c               # Telnet test implementation
│   ├── telnet_interface.h          # Telnet interface abstraction
│   ├── telnet_interface.c          # Telnet interface implementation
│   ├── test_telnet.sh               # Automated telnet test script
│   ├── test_level3.sh              # Level 3 comprehensive test suite
│   ├── benchmark_level3.sh         # Level 3 performance benchmarks
│   └── validate_level3.sh          # Level 3 implementation validation
├── README.md            # This file
├── CLAUDE.md            # AI assistant documentation
├── LOGIC_REVIEW.md      # Logic review and bug fixes
├── LEVEL3_IMPLEMENTATION_SUMMARY.md # Complete Level 3 implementation summary
├── DEPLOYMENT_READINESS_CHECKLIST.md # Production deployment checklist
└── TODO.txt             # Development roadmap
```

## Performance

### Benchmarks

**Level 3 Enhanced Performance:**
- **Latency**: <100ms average processing time (quantum scheduling)
- **Buffer Overflow Rate**: <1% with watermark management
- **Real-time Chat**: Sub-second response for interactive applications
- **1200 bps Optimization**: Enhanced performance for low-speed connections
- **Memory Efficiency**: Dynamic allocation with fragmentation prevention
- **CPU Usage**: <10% idle, <50% load (adaptive scheduling)

**Legacy Performance:**
- **Latency**: < 1ms serial-to-telnet forwarding
- **Throughput**: Supports full baudrate (tested up to 230400 bps)
- **Memory**: ~100KB resident size
- **CPU**: < 1% on modern systems during active transfer

### Limitations

- Single connection at a time (one serial port to one telnet server)
- No modem initialization strings (DIP switches simulation)
- No hardware flow control signals beyond DTR/RTS
- No phone book or stored numbers
- ATD command is a no-op (connection is made via ATA)

## License

This project is provided as-is for educational and personal use. See LICENSE file for details.

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes with descriptive messages
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Development Guidelines

- Follow the existing code style (K&R with 4-space indentation)
- Add comments for complex logic
- Test with both virtual and real serial ports
- Ensure compilation with `-Werror` (warnings as errors)
- Update documentation for new features

## Acknowledgments

- Hayes Microcomputer Products for the AT command standard
- The retrocomputing community for keeping vintage systems alive
- BBS operators worldwide for preserving digital history

## References

- [Hayes AT Command Set](https://en.wikipedia.org/wiki/Hayes_command_set)
- [RFC 854: Telnet Protocol Specification](https://tools.ietf.org/html/rfc854)
- [Serial Programming Guide for POSIX Operating Systems](https://www.cmrr.umn.edu/~strupp/serial.html)
- [Linux Serial HOWTO](https://tldp.org/HOWTO/Serial-HOWTO.html)

## Contact

For bug reports and feature requests, please open an issue on GitHub.

---

**Happy Retrocomputing!** 🖥️📞
