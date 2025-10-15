/*
 * bridge.h - Main bridging logic for ModemBridge
 *
 * Handles bidirectional data transfer between modem and telnet,
 * including ANSI filtering, multibyte character handling, and
 * connection state management
 */

#ifndef MODEMBRIDGE_BRIDGE_H
#define MODEMBRIDGE_BRIDGE_H

#include "common.h"
#include "config.h"
#include "serial.h"
#include "modem.h"
#include "telnet.h"
#include "datalog.h"

/* ANSI escape sequence states */
typedef enum {
    ANSI_STATE_NORMAL,          /* Normal text */
    ANSI_STATE_ESC,             /* Received ESC (0x1B) */
    ANSI_STATE_CSI,             /* Received CSI (ESC [) */
    ANSI_STATE_CSI_PARAM        /* In CSI parameter sequence */
} ansi_state_t;

/* Circular buffer for data transfer */
typedef struct {
    unsigned char data[BUFFER_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t count;
} circular_buffer_t;

/* Bridge context structure */
typedef struct {
    /* Configuration */
    config_t *config;

    /* Serial and modem */
    serial_port_t serial;
    modem_t modem;

    /* Telnet connection */
    telnet_t telnet;

    /* State */
    connection_state_t state;
    bool running;

    /* Data buffers */
    circular_buffer_t serial_to_telnet_buf;
    circular_buffer_t telnet_to_serial_buf;

    /* Multibyte character handling */
    unsigned char mb_buffer[4];     /* Partial multibyte sequence */
    size_t mb_len;                  /* Current partial length */

    /* ANSI processing for modem -> telnet direction */
    ansi_state_t ansi_filter_state;
    char ansi_buffer[SMALL_BUFFER_SIZE];
    size_t ansi_buffer_len;

    /* Statistics */
    uint64_t bytes_serial_to_telnet;
    uint64_t bytes_telnet_to_serial;
    time_t connection_start_time;

    /* Data logging */
    datalog_t datalog;
} bridge_ctx_t;

/* Function prototypes */

/**
 * Initialize bridge context
 * @param ctx Bridge context structure to initialize
 * @param cfg Configuration
 */
void bridge_init(bridge_ctx_t *ctx, config_t *cfg);

/**
 * Start bridge operation
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_start(bridge_ctx_t *ctx);

/**
 * Stop bridge operation
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_stop(bridge_ctx_t *ctx);

/**
 * Main bridge loop (I/O multiplexing)
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_run(bridge_ctx_t *ctx);

/**
 * Process data from serial port
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_process_serial_data(bridge_ctx_t *ctx);

/**
 * Process data from telnet
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_process_telnet_data(bridge_ctx_t *ctx);

/**
 * Handle modem connection establishment
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_modem_connect(bridge_ctx_t *ctx);

/**
 * Handle modem disconnection
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_modem_disconnect(bridge_ctx_t *ctx);

/**
 * Handle telnet connection establishment
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_telnet_connect(bridge_ctx_t *ctx);

/**
 * Handle telnet disconnection
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_telnet_disconnect(bridge_ctx_t *ctx);

/**
 * Print bridge statistics
 * @param ctx Bridge context
 */
void bridge_print_stats(bridge_ctx_t *ctx);

/* Circular buffer functions */

/**
 * Initialize circular buffer
 * @param buf Circular buffer structure
 */
void cbuf_init(circular_buffer_t *buf);

/**
 * Write data to circular buffer
 * @param buf Circular buffer structure
 * @param data Data to write
 * @param len Data length
 * @return Number of bytes written
 */
size_t cbuf_write(circular_buffer_t *buf, const unsigned char *data, size_t len);

/**
 * Read data from circular buffer
 * @param buf Circular buffer structure
 * @param data Buffer to store read data
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
size_t cbuf_read(circular_buffer_t *buf, unsigned char *data, size_t len);

/**
 * Get available data in buffer
 * @param buf Circular buffer structure
 * @return Number of bytes available
 */
size_t cbuf_available(circular_buffer_t *buf);

/**
 * Get free space in buffer
 * @param buf Circular buffer structure
 * @return Number of bytes free
 */
size_t cbuf_free(circular_buffer_t *buf);

/**
 * Check if buffer is empty
 * @param buf Circular buffer structure
 * @return true if empty, false otherwise
 */
bool cbuf_is_empty(circular_buffer_t *buf);

/**
 * Check if buffer is full
 * @param buf Circular buffer structure
 * @return true if full, false otherwise
 */
bool cbuf_is_full(circular_buffer_t *buf);

/**
 * Clear circular buffer
 * @param buf Circular buffer structure
 */
void cbuf_clear(circular_buffer_t *buf);

/* ANSI processing functions */

/**
 * Filter ANSI escape sequences from modem input
 * Removes cursor control and other ANSI codes that shouldn't reach telnet
 * @param input Input data
 * @param input_len Input length
 * @param output Output buffer for filtered data
 * @param output_size Output buffer size
 * @param output_len Pointer to store actual output length
 * @param state ANSI parser state (maintained across calls)
 * @return SUCCESS on success, error code on failure
 */
int ansi_filter_modem_to_telnet(const unsigned char *input, size_t input_len,
                                unsigned char *output, size_t output_size,
                                size_t *output_len, ansi_state_t *state);

/**
 * Pass through ANSI escape sequences from telnet to modem
 * @param input Input data
 * @param input_len Input length
 * @param output Output buffer
 * @param output_size Output buffer size
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, error code on failure
 */
int ansi_passthrough_telnet_to_modem(const unsigned char *input, size_t input_len,
                                     unsigned char *output, size_t output_size,
                                     size_t *output_len);

/* Multibyte character handling */

/**
 * Check if byte is start of multibyte UTF-8 sequence
 * @param byte Byte to check
 * @return true if multibyte start, false otherwise
 */
bool is_utf8_start(unsigned char byte);

/**
 * Check if byte is UTF-8 continuation byte
 * @param byte Byte to check
 * @return true if continuation byte, false otherwise
 */
bool is_utf8_continuation(unsigned char byte);

/**
 * Get expected length of UTF-8 sequence from first byte
 * @param byte First byte of sequence
 * @return Expected sequence length (1-4), or 0 if invalid
 */
int utf8_sequence_length(unsigned char byte);

/**
 * Validate complete UTF-8 sequence
 * @param seq Byte sequence
 * @param len Sequence length
 * @return true if valid, false otherwise
 */
bool is_valid_utf8_sequence(const unsigned char *seq, size_t len);

#endif /* MODEMBRIDGE_BRIDGE_H */
