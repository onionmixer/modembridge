/*
 * level3_types.h - Type definitions for Level 3 Pipeline Management
 *
 * This file contains all type definitions (enums, structs) for Level 3,
 * separated to avoid circular dependencies between header files.
 */

#ifndef MODEMBRIDGE_LEVEL3_TYPES_H
#define MODEMBRIDGE_LEVEL3_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/* Level 3 Pipeline Configuration - Using common utility constants */
#define L3_PIPELINE_BUFFER_SIZE        4096                       /* Buffer size per pipeline */
#define L3_MAX_BURST_SIZE             4096                       /* Maximum burst size */
#define L3_FAIRNESS_TIME_SLICE_MS     50                          /* Time slice per pipeline (ms) */
#define L3_BACKPRESSURE_TIMEOUT_MS    5000                        /* Backpressure timeout (ms) */

/* Level 3 State Machine Timeouts */
#define LEVEL3_CONNECT_TIMEOUT        30                          /* Connection timeout (seconds) */
#define LEVEL3_SHUTDOWN_TIMEOUT       10                          /* Shutdown timeout (seconds) */
#define LEVEL3_INIT_TIMEOUT           15                          /* Initialization timeout (seconds) */

/* Enhanced Backpressure Watermarks */
#define L3_CRITICAL_WATERMARK         (L3_PIPELINE_BUFFER_SIZE * 0.95)  /* 95% - Emergency stop */
#define L3_HIGH_WATERMARK             (L3_PIPELINE_BUFFER_SIZE * 0.80)  /* 80% - Apply backpressure */
#define L3_LOW_WATERMARK              (L3_PIPELINE_BUFFER_SIZE * 0.20)  /* 20% - Release backpressure */
#define L3_EMPTY_WATERMARK            (L3_PIPELINE_BUFFER_SIZE * 0.05)  /* 5% - Buffer empty */

/* Number of pipeline directions */
#define LEVEL3_DIRECTION_COUNT 3          /* Total number of directions (1-based enum + 1) */

/* Level 3 Special Return Values */
#define L3_QUANTUM_EXPIRED        1       /* Quantum time slice expired */

/* Pipeline Directions */
typedef enum {
    L3_PIPELINE_SERIAL_TO_TELNET = 1,    /* Pipeline 1: Serial → Telnet */
    L3_PIPELINE_TELNET_TO_SERIAL = 2     /* Pipeline 2: Telnet → Serial */
} l3_pipeline_direction_t;

/* Legacy direction type for backward compatibility */
typedef l3_pipeline_direction_t l3_direction_t;

/* Enhanced Level 3 System States */
typedef enum {
    /* Initial System States */
    L3_STATE_UNINITIALIZED = 0,          /* System not initialized */
    L3_STATE_INITIALIZING,               /* Initialization in progress */

    /* Connection/Ready States */
    L3_STATE_READY,                      /* Ready for connection (command mode) */
    L3_STATE_CONNECTING,                 /* Establishing connection */
    L3_STATE_NEGOTIATING,                /* Protocol negotiation active */

    /* Active Data Transfer States */
    L3_STATE_DATA_TRANSFER,              /* Active data transfer (online mode) */
    L3_STATE_FLUSHING,                   /* Flushing pending data */

    /* Shutdown/Termination States */
    L3_STATE_SHUTTING_DOWN,              /* Graceful shutdown in progress */
    L3_STATE_TERMINATED,                 /* System terminated */
    L3_STATE_ERROR                       /* Error state requiring recovery */
} l3_system_state_t;

/* Level 3 Result Codes */
typedef enum {
    L3_SUCCESS = 0,                     /* Success */
    L3_ERROR_FAILURE = -1,               /* General failure */
    L3_ERROR_INVALID_PARAM = -2,         /* Invalid parameter */
    L3_ERROR_TIMEOUT = -3,               /* Timeout occurred */
    L3_ERROR_BUFFER_FULL = -4,           /* Buffer is full */
    L3_ERROR_INVALID_STATE = -5,         /* Invalid state transition */
    L3_ERROR_BUSY = -6,                  /* Resource busy */
    L3_ERROR_MEMORY = -7,                /* Memory allocation failed */
    L3_ERROR_IO = -8,                    /* I/O error */
    L3_ERROR_THREAD = -9,                /* Thread error */
    L3_ERROR_QUEUE_FULL = -10,           /* Queue is full */
    L3_ERROR_NO_VIOLATION = -11          /* No latency violation detected */
} l3_result_t;

/* Pipeline States */
typedef enum {
    L3_PIPELINE_STATE_IDLE = 0,          /* No active data transfer */
    L3_PIPELINE_STATE_ACTIVE,            /* Currently processing data */
    L3_PIPELINE_STATE_BLOCKED,           /* Blocked by backpressure */
    L3_PIPELINE_STATE_ERROR              /* Error condition */
} l3_pipeline_state_t;

/* Buffer Watermark Levels */
typedef enum {
    L3_WATERMARK_CRITICAL = 0,      /* Buffer almost full (>95%) */
    L3_WATERMARK_HIGH = 1,          /* High water mark (>80%) */
    L3_WATERMARK_NORMAL = 2,        /* Normal range (20-80%) */
    L3_WATERMARK_LOW = 3,           /* Low water mark (<20%) */
    L3_WATERMARK_EMPTY = 4          /* Buffer empty (<5%) */
} l3_watermark_level_t;

/* Hayes Command Types */
typedef enum {
    HAYES_CMD_BASIC = 0,                 /* Basic AT command (ATE, ATH, etc.) */
    HAYES_CMD_EXTENDED,                  /* Extended AT& command */
    HAYES_CMD_REGISTER,                  /* S-register command (ATS) */
    HAYES_CMD_PROPRIETARY                /* Vendor-specific command */
} hayes_command_type_t;

/* Hayes Command Filter State */
typedef enum {
    HAYES_STATE_NORMAL = 0,              /* Normal data mode */
    HAYES_STATE_ESCAPE,                  /* Received ESC (0x1B) */
    HAYES_STATE_PLUS_ESCAPE,             /* Detecting +++ escape sequence */
    HAYES_STATE_COMMAND,                 /* In AT command mode */
    HAYES_STATE_RESULT,                  /* Processing result code */
    HAYES_STATE_CR_WAIT,                 /* Waiting for CR after command */
    HAYES_STATE_LF_WAIT                  /* Waiting for LF after CR */
} hayes_filter_state_t;

/* TELNET Control Code Filter State */
typedef enum {
    TELNET_FILTER_STATE_DATA = 0,        /* Normal data */
    TELNET_FILTER_STATE_IAC,             /* Received IAC (0xFF) */
    TELNET_FILTER_STATE_WILL,            /* WILL option */
    TELNET_FILTER_STATE_WONT,            /* WONT option */
    TELNET_FILTER_STATE_DO,              /* DO option */
    TELNET_FILTER_STATE_DONT,            /* DONT option */
    TELNET_FILTER_STATE_SB,              /* Suboption begin */
    TELNET_FILTER_STATE_SB_DATA          /* Suboption data */
} telnet_filter_state_t;

#endif /* MODEMBRIDGE_LEVEL3_TYPES_H */