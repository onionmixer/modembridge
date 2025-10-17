/*
 * serial.h - Serial port communication module for ModemBridge
 *
 * Handles low-level serial port operations using termios API
 */

#ifndef MODEMBRIDGE_SERIAL_H
#define MODEMBRIDGE_SERIAL_H

#include "common.h"
#include "config.h"
#include <termios.h>

/* Serial port configuration for Level 3 */
typedef struct {
    speed_t fixed_dte_speed;        /* Fixed DTE speed (host↔modem) */
    bool use_fixed_speed;           /* Use fixed speed instead of dynamic */
    bool hardware_flow_control;     /* RTS/CTS hardware flow control enabled */
    bool software_flow_control;     /* XON/XOFF software flow control enabled */
    int xon_char;                   /* XON character (default 0x11) */
    int xoff_char;                  /* XOFF character (default 0x13) */
    bool low_speed_optimization;    /* Enable 1200 bps optimizations */
    size_t tx_buffer_size;          /* TX buffer size for low speed */
    size_t rx_buffer_size;          /* RX buffer size for low speed */
} serial_level3_config_t;

/* Serial port handle structure */
typedef struct {
    int fd;                         /* File descriptor */
    int epoll_fd;                   /* epoll instance for this serial port */
    char device[SMALL_BUFFER_SIZE]; /* Device path (e.g., /dev/ttyUSB0) */
    struct termios oldtio;          /* Original terminal settings */
    struct termios newtio;          /* Current terminal settings */
    speed_t baudrate;               /* Current baudrate */
    bool is_open;                   /* Open status flag */

    /* Level 3 speed control and flow management */
    serial_level3_config_t l3_config; /* Level 3 configuration */

    /* Flow control state */
    bool tx_blocked;                /* TX blocked by flow control */
    bool rx_blocked;                /* RX blocked by flow control */
    time_t last_xoff_time;          /* Last XOFF received time */
    size_t tx_flow_watermark;       /* TX flow control high watermark */
    size_t rx_flow_watermark;       /* RX flow control high watermark */
} serial_port_t;

/* Function prototypes */

/**
 * Initialize serial port structure
 * @param port Serial port structure to initialize
 */
void serial_init(serial_port_t *port);

/**
 * Open serial port with configuration
 * @param port Serial port structure
 * @param device Device path (e.g., /dev/ttyUSB0)
 * @param cfg Configuration structure
 * @return SUCCESS on success, error code on failure
 */
int serial_open(serial_port_t *port, const char *device, const config_t *cfg);

/**
 * Close serial port
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_close(serial_port_t *port);

/**
 * Configure serial port parameters
 * @param port Serial port structure
 * @param baudrate Baudrate (termios speed_t)
 * @param parity Parity setting
 * @param data_bits Data bits (7 or 8)
 * @param stop_bits Stop bits (1 or 2)
 * @param flow Flow control setting
 * @return SUCCESS on success, error code on failure
 */
int serial_configure(serial_port_t *port, speed_t baudrate, parity_t parity,
                     int data_bits, int stop_bits, flow_control_t flow);

/**
 * Change baudrate dynamically
 * @param port Serial port structure
 * @param baudrate New baudrate (termios speed_t)
 * @return SUCCESS on success, error code on failure
 */
int serial_set_baudrate(serial_port_t *port, speed_t baudrate);

/**
 * Read data from serial port (non-blocking)
 * @param port Serial port structure
 * @param buffer Buffer to store read data
 * @param size Maximum bytes to read
 * @return Number of bytes read, or error code on failure
 */
ssize_t serial_read(serial_port_t *port, void *buffer, size_t size);

/**
 * Write data to serial port
 * @param port Serial port structure
 * @param buffer Data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, or error code on failure
 */
ssize_t serial_write(serial_port_t *port, const void *buffer, size_t size);

/**
 * Write data to serial port using epoll for write readiness
 * Uses EPOLLOUT to check if port is ready for writing before attempting write
 * Handles partial writes automatically with retry logic
 * @param port Serial port structure
 * @param buffer Data to write
 * @param size Number of bytes to write
 * @param timeout_ms Timeout in milliseconds for epoll_wait (0 = no wait, -1 = infinite)
 * @return Number of bytes written, or error code on failure
 */
ssize_t serial_write_with_epoll(serial_port_t *port, const void *buffer, size_t size, int timeout_ms);

/**
 * Flush serial port buffers
 * @param port Serial port structure
 * @param queue_selector TCIFLUSH, TCOFLUSH, or TCIOFLUSH
 * @return SUCCESS on success, error code on failure
 */
int serial_flush(serial_port_t *port, int queue_selector);

/**
 * Set DTR (Data Terminal Ready) signal
 * @param port Serial port structure
 * @param state true to set, false to clear
 * @return SUCCESS on success, error code on failure
 */
int serial_set_dtr(serial_port_t *port, bool state);

/**
 * Set RTS (Request To Send) signal
 * @param port Serial port structure
 * @param state true to set, false to clear
 * @return SUCCESS on success, error code on failure
 */
int serial_set_rts(serial_port_t *port, bool state);

/**
 * Get DCD (Data Carrier Detect) signal state
 * @param port Serial port structure
 * @return 1 if DCD is high, 0 if low, -1 on error
 */
int serial_get_dcd(serial_port_t *port);

/**
 * Get DSR (Data Set Ready) signal state
 * @param port Serial port structure
 * @param state Pointer to store state
 * @return SUCCESS on success, error code on failure
 */
int serial_get_dsr(serial_port_t *port, bool *state);

/**
 * Get CTS (Clear To Send) signal state
 * @param port Serial port structure
 * @param state Pointer to store state
 * @return SUCCESS on success, error code on failure
 */
int serial_get_cts(serial_port_t *port, bool *state);

/**
 * Get file descriptor for select/poll
 * @param port Serial port structure
 * @return File descriptor, or -1 if not open
 */
int serial_get_fd(serial_port_t *port);

/**
 * Check if serial port is open
 * @param port Serial port structure
 * @return true if open, false otherwise
 */
bool serial_is_open(serial_port_t *port);

/* ===== Extended functions from modem_sample ===== */

/**
 * Read a line from serial port (until \r or \n)
 * Uses internal buffering to handle fragmented data
 * Based on modem_sample/serial_port.c
 * @param port Serial port structure
 * @param buffer Buffer to store line
 * @param size Maximum buffer size
 * @param timeout_sec Timeout in seconds
 * @return Number of bytes read (excluding line terminator), or error code on failure
 */
ssize_t serial_read_line(serial_port_t *port, char *buffer, size_t size, int timeout_sec);

/**
 * Lock serial port using UUCP-style lock file
 * Creates /var/lock/LCK..ttyUSB0 style lock file
 * Based on modem_sample/serial_port.c
 * @param device Device path (e.g., /dev/ttyUSB0)
 * @return SUCCESS on success, error code on failure
 */
int serial_lock_port(const char *device);

/**
 * Unlock serial port
 * Removes UUCP-style lock file
 * Based on modem_sample/serial_port.c
 */
void serial_unlock_port(void);

/**
 * Enable carrier detect (DCD) monitoring
 * Clears CLOCAL flag to enable carrier detect
 * Based on modem_sample/serial_port.c:enable_carrier_detect()
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_enable_carrier_detect(serial_port_t *port);

/**
 * Disable carrier detect (DCD) monitoring
 * Sets CLOCAL flag to ignore carrier
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_disable_carrier_detect(serial_port_t *port);

/**
 * Check carrier (DCD) status
 * Based on modem_sample/serial_port.c:check_carrier_status()
 * @param port Serial port structure
 * @param carrier Pointer to store carrier state (true = present, false = lost)
 * @return SUCCESS on success, error code on failure
 */
int serial_check_carrier(serial_port_t *port, bool *carrier);

/**
 * Write data with robust error handling and retry logic
 * Based on modem_sample/serial_port.c:robust_serial_write()
 * Features:
 * - Carrier check before transmission
 * - Partial write handling
 * - Retry on EAGAIN/EWOULDBLOCK (max 3 times)
 * - EPIPE/ECONNRESET detection
 * - tcdrain() after write
 * @param port Serial port structure
 * @param buffer Data to write
 * @param size Number of bytes to write
 * @return Number of bytes written, or error code on failure
 */
ssize_t serial_write_robust(serial_port_t *port, const void *buffer, size_t size);

/**
 * Perform DTR drop hangup
 * Based on modem_sample/serial_port.c:dtr_drop_hangup()
 * Sets baudrate to B0 to drop DTR signal, waits 1 second, then restores
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_dtr_drop_hangup(serial_port_t *port);

/**
 * Buffered serial transmission for large data
 * Based on modem_sample/serial_port.c:buffered_serial_send()
 * Splits large data into chunks (TX_CHUNK_SIZE) with delays (TX_CHUNK_DELAY_US)
 * Prevents receiver buffer overflow with slow modems
 * @param port Serial port structure
 * @param buffer Data to transmit
 * @param size Total size of data
 * @return Number of bytes sent, or error code on failure
 */
ssize_t serial_write_buffered(serial_port_t *port, const void *buffer, size_t size);

/* ===== Level 3 Speed Control and Flow Management Functions ===== */

/**
 * Initialize Level 3 serial configuration with defaults
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_init_level3_config(serial_port_t *port);

/**
 * Configure fixed DTE speed for Level 3 operations
 * @param port Serial port structure
 * @param fixed_speed Fixed speed (e.g., B57600, B115200)
 * @param enable Enable or disable fixed speed mode
 * @return SUCCESS on success, error code on failure
 */
int serial_set_fixed_dte_speed(serial_port_t *port, speed_t fixed_speed, bool enable);

/**
 * Configure hardware flow control (RTS/CTS)
 * @param port Serial port structure
 * @param enable Enable or disable hardware flow control
 * @return SUCCESS on success, error code on failure
 */
int serial_set_hardware_flow_control(serial_port_t *port, bool enable);

/**
 * Configure software flow control (XON/XOFF)
 * @param port Serial port structure
 * @param enable Enable or disable software flow control
 * @param xon_char XON character (0x11 default)
 * @param xoff_char XOFF character (0x13 default)
 * @return SUCCESS on success, error code on failure
 */
int serial_set_software_flow_control(serial_port_t *port, bool enable,
                                   int xon_char, int xoff_char);

/**
 * Enable low-speed optimizations for 1200 bps connections
 * @param port Serial port structure
 * @param enable Enable or disable low-speed optimizations
 * @return SUCCESS on success, error code on failure
 */
int serial_enable_low_speed_optimization(serial_port_t *port, bool enable);

/**
 * Apply Level 3 configuration to serial port
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_apply_level3_config(serial_port_t *port);

/**
 * Handle incoming flow control characters (XON/XOFF)
 * @param port Serial port structure
 * @param data Received data buffer
 * @param len Data length
 * @return SUCCESS on success, error code on failure
 */
int serial_handle_flow_control(serial_port_t *port, const char *data, size_t len);

/**
 * Check if TX is blocked by flow control
 * @param port Serial port structure
 * @return true if TX is blocked
 */
bool serial_is_tx_blocked(serial_port_t *port);

/**
 * Check if RX is blocked by flow control
 * @param port Serial port structure
 * @return true if RX is blocked
 */
bool serial_is_rx_blocked(serial_port_t *port);

/**
 * Send XON character to resume flow
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_send_xon(serial_port_t *port);

/**
 * Send XOFF character to pause flow
 * @param port Serial port structure
 * @return SUCCESS on success, error code on failure
 */
int serial_send_xoff(serial_port_t *port);

/**
 * Get optimal buffer size for current speed and flow control
 * @param port Serial port structure
 * @param is_tx True for TX buffer, False for RX buffer
 * @return Optimal buffer size in bytes
 */
size_t serial_get_optimal_buffer_size(serial_port_t *port, bool is_tx);

/**
 * Calculate transmission delay for current baudrate
 * @param port Serial port structure
 * @param bytes Number of bytes to transmit
 * @return Delay in microseconds
 */
useconds_t serial_calculate_tx_delay(serial_port_t *port, size_t bytes);

/* ===== Level 1 Dynamic Baudrate Adjustment Functions ===== */

/**
 * Dynamically adjust serial port speed based on modem connection
 * @param port Serial port structure
 * @param target_speed Target speed in baud
 * @return SUCCESS on success, error code on failure
 */
int serial_adjust_baudrate_dynamically(serial_port_t *port, int target_speed);

/**
 * Validate serial port speed is within supported range
 * @param speed Speed to validate
 * @return true if valid, false otherwise
 */
bool serial_is_valid_speed(int speed);

/**
 * Convert baudrate integer to speed_t (termios constant)
 * @param baudrate Baudrate as integer (e.g., 9600, 115200)
 * @return termios speed_t constant (e.g., B9600, B115200)
 */
speed_t serial_baudrate_to_speed_t(int baudrate);

/**
 * Check if data is available in serial input buffer
 * @param port Serial port structure
 * @return true if data is available, false otherwise
 */
bool serial_check_available(serial_port_t *port);

#endif /* MODEMBRIDGE_SERIAL_H */
