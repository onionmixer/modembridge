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
#ifdef ENABLE_LEVEL2
#include "telnet.h"
#endif
#include "datalog.h"
#include "timestamp.h"
#include "echo.h"
#include <pthread.h>
#include <time.h>


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

/* Thread-safe circular buffer for multithread mode */
typedef struct {
    circular_buffer_t cbuf;             /* Underlying circular buffer */
    pthread_mutex_t mutex;              /* Buffer access synchronization */
    pthread_cond_t cond_not_empty;      /* Data available signal */
    pthread_cond_t cond_not_full;       /* Space available signal */
} ts_circular_buffer_t;

/* Bridge context structure */
typedef struct {
    /* Configuration */
    config_t *config;

    /* Serial and modem */
    serial_port_t serial;
    modem_t modem;

  #ifdef ENABLE_LEVEL2
    /* Telnet connection */
    telnet_t telnet;
#endif

    /* State */
    connection_state_t state;
    bool running;

    /* Resource availability flags */
    bool serial_ready;           /* Serial port available and open */
    bool modem_ready;            /* Modem initialized successfully */

    /* Connection state (modem_sample integration) */
    int connected_baudrate;      /* Negotiated connection baudrate (from CONNECT response) */
    bool carrier_detected;       /* Current carrier detect status */
    int ring_count;              /* Number of RINGs detected */

    /* Retry state */
    time_t last_serial_retry;    /* Last serial port retry attempt */
    int serial_retry_interval;   /* Serial retry interval in seconds (default 10) */
    int serial_retry_count;      /* Number of serial retry attempts (for logging) */

    /* Data buffers */
    circular_buffer_t serial_to_telnet_buf;
    circular_buffer_t telnet_to_serial_buf;

    /* Thread handles (multithread mode) */
    pthread_t serial_thread;
#ifdef ENABLE_LEVEL2
    pthread_t telnet_thread;
#endif
#ifdef ENABLE_LEVEL3
    pthread_t level3_thread;
#endif

    /* Thread-safe buffers (will replace circular_buffer_t in multithread mode) */
    ts_circular_buffer_t ts_serial_to_telnet_buf;
    ts_circular_buffer_t ts_telnet_to_serial_buf;

    /* Shared state protection */
    pthread_mutex_t state_mutex;        /* Protect connection_state_t */
    pthread_mutex_t modem_mutex;        /* Protect modem_t state */

    /* Thread control */
    bool thread_running;                /* Thread execution flag */

    /* Client connection status (Level 1) */
    bool client_data_received;          /* Flag: true after receiving first data from client */

    /* Timestamp transmission control (Level 1) */
    timestamp_ctrl_t timestamp;

    /* Echo functionality control (Level 1) */
    echo_ctrl_t echo;

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

#ifdef ENABLE_LEVEL3
    /* Level 3 pipeline management - using opaque pointer to avoid circular dependency */
    void *level3;                       /* Pointer to l3_context_t */
    bool level3_enabled;                 /* Level 3 system enabled flag */
#endif

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

#ifdef ENABLE_LEVEL2
/**
 * Process data from telnet (Level 2 only)
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_process_telnet_data(bridge_ctx_t *ctx);

/**
 * Process data from serial port - Level 2 exclusive implementation
 * Handles serial I/O errors and transitions to DISCONNECTED state
 * Focuses on telnet bridging functionality with minimal Level 1 sharing
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_process_serial_data_level2(bridge_ctx_t *ctx);
#endif

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

#ifdef ENABLE_LEVEL2
/**
 * Handle telnet connection establishment (Level 2 only)
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_telnet_connect(bridge_ctx_t *ctx);

/**
 * Handle telnet disconnection (Level 2 only)
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_telnet_disconnect(bridge_ctx_t *ctx);
#endif

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

/* Thread-safe circular buffer functions */

/**
 * Initialize thread-safe circular buffer
 * @param tsbuf Thread-safe circular buffer structure
 */
void ts_cbuf_init(ts_circular_buffer_t *tsbuf);

/**
 * Destroy thread-safe circular buffer (cleanup mutexes and condition variables)
 * @param tsbuf Thread-safe circular buffer structure
 */
void ts_cbuf_destroy(ts_circular_buffer_t *tsbuf);

/**
 * Write data to thread-safe circular buffer (non-blocking)
 * @param tsbuf Thread-safe circular buffer structure
 * @param data Data to write
 * @param len Data length
 * @return Number of bytes written (may be less than len if buffer is full)
 */
size_t ts_cbuf_write(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len);

/**
 * Read data from thread-safe circular buffer (non-blocking)
 * @param tsbuf Thread-safe circular buffer structure
 * @param data Buffer to store read data
 * @param len Maximum bytes to read
 * @return Number of bytes read (may be 0 if buffer is empty)
 */
size_t ts_cbuf_read(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len);

/**
 * Write data with timeout (blocking until space available or timeout)
 * @param tsbuf Thread-safe circular buffer structure
 * @param data Data to write
 * @param len Data length
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes written, or 0 on timeout
 */
size_t ts_cbuf_write_timeout(ts_circular_buffer_t *tsbuf, const unsigned char *data,
                              size_t len, int timeout_ms);

/**
 * Read data with timeout (blocking until data available or timeout)
 * @param tsbuf Thread-safe circular buffer structure
 * @param data Buffer to store read data
 * @param len Maximum bytes to read
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes read, or 0 on timeout
 */
size_t ts_cbuf_read_timeout(ts_circular_buffer_t *tsbuf, unsigned char *data,
                             size_t len, int timeout_ms);

/**
 * Check if buffer is empty (thread-safe)
 * @param tsbuf Thread-safe circular buffer structure
 * @return true if empty, false otherwise
 */
bool ts_cbuf_is_empty(ts_circular_buffer_t *tsbuf);

/**
 * Get available data in buffer (thread-safe)
 * @param tsbuf Thread-safe circular buffer structure
 * @return Number of bytes available
 */
size_t ts_cbuf_available(ts_circular_buffer_t *tsbuf);

/* Thread functions for multithread mode */

/**
 * Serial/Modem thread function (Level 1)
 * Handles serial I/O, modem processing, and Serial<->Telnet buffering
 * @param arg Pointer to bridge_ctx_t
 * @return NULL
 */
void *serial_modem_thread_func(void *arg);

#ifdef ENABLE_LEVEL2
/**
 * Telnet thread function (Level 2 only)
 * Handles telnet I/O, IAC processing, and Telnet<->Serial buffering
 * @param arg Pointer to bridge_ctx_t
 * @return NULL
 */
void *telnet_thread_func(void *arg);
#endif

#ifdef ENABLE_LEVEL3
/**
 * Level 3 management functions
 */

/**
 * Initialize Level 3 pipeline system
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_init_level3(bridge_ctx_t *ctx);

/**
 * Start Level 3 pipeline management
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_start_level3(bridge_ctx_t *ctx);

/**
 * Stop Level 3 pipeline management
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_stop_level3(bridge_ctx_t *ctx);

/**
 * Check if Level 3 should be enabled based on configuration and system state
 * @param ctx Bridge context
 * @return true if Level 3 should be enabled
 */
bool bridge_should_enable_level3(bridge_ctx_t *ctx);

/**
 * Level 3 thread function
 * Manages dual pipeline system with fair scheduling and backpressure
 * @param arg Pointer to bridge_ctx_t
 * @return NULL
 */
void *bridge_level3_thread_func(void *arg);

/**
 * Handle DCD (Data Carrier Detect) state changes from modem
 * This function bridges DCD events between Level 1 (modem) and Level 3 (pipeline)
 * @param ctx Bridge context
 * @param dcd_state New DCD state (true = risen, false = fell)
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_dcd_event(bridge_ctx_t *ctx, bool dcd_state);

/**
 * Get current DCD state from bridge
 * @param ctx Bridge context
 * @return Current DCD state (true = active, false = inactive)
 */
bool bridge_get_dcd_state(bridge_ctx_t *ctx);

/**
 * Check if bridge should notify Level 3 of DCD events
 * @param ctx Bridge context
 * @return true if Level 3 is active and ready to receive DCD events
 */
bool bridge_should_notify_level3_dcd(bridge_ctx_t *ctx);
#endif


#ifdef ENABLE_LEVEL3
#include "level3.h"
#endif

#endif /* MODEMBRIDGE_BRIDGE_H */
