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

/* Serial port handle structure */
typedef struct {
    int fd;                         /* File descriptor */
    char device[SMALL_BUFFER_SIZE]; /* Device path (e.g., /dev/ttyUSB0) */
    struct termios oldtio;          /* Original terminal settings */
    struct termios newtio;          /* Current terminal settings */
    speed_t baudrate;               /* Current baudrate */
    bool is_open;                   /* Open status flag */
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
 * @param state Pointer to store state
 * @return SUCCESS on success, error code on failure
 */
int serial_get_dcd(serial_port_t *port, bool *state);

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

#endif /* MODEMBRIDGE_SERIAL_H */
