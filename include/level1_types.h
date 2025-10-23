/*
 * level1_types.h - Type definitions for Level 1 (Serial/Modem) functionality
 *
 * This file contains all type definitions, enums, and structures used by
 * Level 1 components of ModemBridge. These types are extracted from bridge.h
 * to create a cleaner separation between Level 1 and higher-level functionality.
 */

#ifndef MODEMBRIDGE_LEVEL1_TYPES_H
#define MODEMBRIDGE_LEVEL1_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/* ========== Buffer Size Constants ========== */

/* Default buffer size for circular buffers (4KB) */
#ifndef BUFFER_SIZE
#define BUFFER_SIZE         4096
#endif

/* Maximum line buffer size for serial data */
#define MAX_LINE_BUFFER     1024

/* ========== UTF-8 Constants ========== */

/* UTF-8 byte markers */
#define UTF8_CONT_MASK      0x80    /* 10xxxxxx - continuation byte */
#define UTF8_CONT_MARKER    0x80
#define UTF8_2BYTE_MASK     0xE0    /* 110xxxxx - 2-byte sequence */
#define UTF8_2BYTE_MARKER   0xC0
#define UTF8_3BYTE_MASK     0xF0    /* 1110xxxx - 3-byte sequence */
#define UTF8_3BYTE_MARKER   0xE0
#define UTF8_4BYTE_MASK     0xF8    /* 11110xxx - 4-byte sequence */
#define UTF8_4BYTE_MARKER   0xF0

/* Maximum UTF-8 sequence length */
#define UTF8_MAX_LENGTH     4

/* ========== ANSI Escape Sequence Types ========== */

/**
 * ANSI escape sequence parser states
 * Used for filtering ANSI control sequences from modem to telnet
 */
typedef enum {
    ANSI_STATE_NORMAL,          /* Normal text, not in escape sequence */
    ANSI_STATE_ESC,             /* Received ESC (0x1B) */
    ANSI_STATE_CSI,             /* Received CSI (ESC [) */
    ANSI_STATE_CSI_PARAM        /* In CSI parameter sequence */
} ansi_state_t;

/* ANSI escape character */
#define ANSI_ESC            0x1B

/* ANSI CSI (Control Sequence Introducer) */
#define ANSI_CSI_OPENER     '['

/* ========== Circular Buffer Types ========== */

/**
 * Basic circular buffer for single-threaded operation
 * Used for buffering data between serial and telnet in Level 1 mode
 */
typedef struct {
    unsigned char data[BUFFER_SIZE];    /* Buffer storage */
    size_t read_pos;                    /* Read position (consumer) */
    size_t write_pos;                   /* Write position (producer) */
    size_t count;                       /* Number of bytes in buffer */
} circular_buffer_t;

/**
 * Thread-safe circular buffer for multi-threaded operation
 * Adds synchronization primitives to the basic circular buffer
 */
typedef struct {
    circular_buffer_t cbuf;             /* Underlying circular buffer */
    pthread_mutex_t mutex;              /* Buffer access synchronization */
    pthread_cond_t cond_not_empty;      /* Signal: data available for reading */
    pthread_cond_t cond_not_full;       /* Signal: space available for writing */
    bool initialized;                   /* Initialization flag */
} ts_circular_buffer_t;

/* ========== Thread Control Types ========== */

/**
 * Thread state for Level 1 serial/modem thread
 */
typedef enum {
    THREAD_STATE_STOPPED,       /* Thread not running */
    THREAD_STATE_STARTING,      /* Thread is starting up */
    THREAD_STATE_RUNNING,       /* Thread is running normally */
    THREAD_STATE_STOPPING,      /* Thread is shutting down */
    THREAD_STATE_ERROR          /* Thread encountered an error */
} thread_state_t;

/* ========== Data Processing Types ========== */

/**
 * Result codes for Level 1 data processing functions
 */
typedef enum {
    L1_SUCCESS = 0,             /* Operation successful */
    L1_ERROR = -1,              /* General error */
    L1_ERROR_INVALID_ARG = -2,  /* Invalid argument */
    L1_ERROR_BUFFER_FULL = -3,  /* Buffer is full */
    L1_ERROR_BUFFER_EMPTY = -4, /* Buffer is empty */
    L1_ERROR_TIMEOUT = -5,      /* Operation timed out */
    L1_ERROR_DISCONNECTED = -6, /* Connection lost */
    L1_ERROR_PARTIAL = -7       /* Partial data processed */
} level1_result_t;

/* ========== Echo Processing Types ========== */

/**
 * Echo mode for Level 1 serial processing
 */
typedef enum {
    ECHO_MODE_NONE,             /* No echo */
    ECHO_MODE_LOCAL,            /* Local echo only */
    ECHO_MODE_REMOTE,           /* Remote echo only */
    ECHO_MODE_BOTH              /* Both local and remote echo */
} echo_mode_t;

/* ========== Timestamp Types ========== */

/**
 * Timestamp configuration for Level 1
 */
typedef struct {
    bool enabled;               /* Enable timestamp transmission */
    int interval_seconds;       /* Interval between timestamps */
    time_t last_sent;          /* Last timestamp sent time */
    char format[64];           /* Timestamp format string */
} timestamp_config_t;

/* ========== Serial Data Processing Context ========== */

/**
 * Context for Level 1 serial data processing
 * Contains state needed for processing serial data independently
 */
typedef struct {
    /* Line buffering for serial data */
    unsigned char line_buffer[MAX_LINE_BUFFER];
    size_t line_buffer_pos;
    bool in_command_mode;       /* True if modem is in command mode */

    /* ANSI filtering state */
    ansi_state_t ansi_state;

    /* UTF-8 processing state */
    unsigned char utf8_buffer[UTF8_MAX_LENGTH];
    size_t utf8_buffer_len;
    bool utf8_sequence_started;

    /* Echo processing */
    echo_mode_t echo_mode;
    bool echo_enabled;

    /* Statistics */
    size_t bytes_received;
    size_t bytes_sent;
    size_t packets_processed;

} level1_serial_context_t;

/* ========== Function Result Codes ========== */

/* Success code */
#define L1_OK               0

/* Error codes (compatible with existing ERROR_* codes) */
#define L1_ERROR_GENERIC    -1
#define L1_ERROR_INVALID   -2
#define L1_ERROR_TIMEOUT   -3
#define L1_ERROR_FULL      -4
#define L1_ERROR_EMPTY     -5

#endif /* MODEMBRIDGE_LEVEL1_TYPES_H */