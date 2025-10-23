/*
 * level2_types.h - Level 2 (Telnet Layer) Type Definitions
 *
 * This header defines types specific to Level 2 (Telnet) functionality.
 * Level 2 handles all telnet protocol operations including:
 * - Telnet connection management
 * - IAC protocol processing
 * - Bidirectional data transfer between serial and telnet
 * - Echo mode synchronization
 */

#ifndef LEVEL2_TYPES_H
#define LEVEL2_TYPES_H

#ifdef ENABLE_LEVEL2

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ========================================================================
 * Level 2 Constants and Macros
 * ========================================================================*/

/* Buffer size for Level 2 operations */
#ifndef BUFFER_SIZE
#define BUFFER_SIZE 4096
#endif

/* Telnet thread sleep intervals */
#define LEVEL2_THREAD_SLEEP_SHORT   1000     /* 1ms - for active connection */
#define LEVEL2_THREAD_SLEEP_MEDIUM  10000    /* 10ms - for normal operation */
#define LEVEL2_THREAD_SLEEP_LONG    100000   /* 100ms - for idle/disconnect */

/* Connection retry parameters */
#define LEVEL2_CONNECT_RETRY_DELAY  100000   /* 100ms between connection retries */
#define LEVEL2_EVENT_TIMEOUT         100     /* 100ms timeout for event processing */

/* Level 2 operation modes */
#define LEVEL2_MODE_BRIDGE    0    /* Normal bridging mode */
#define LEVEL2_MODE_PASSTHRU  1    /* Pass-through mode (no filtering) */

/* ========================================================================
 * Level 2 State Definitions
 * ========================================================================*/

/**
 * Level 2 connection states
 * These states are specific to telnet connection management
 */
typedef enum {
    L2_STATE_DISCONNECTED = 0,  /* No telnet connection */
    L2_STATE_CONNECTING,         /* Telnet connection in progress */
    L2_STATE_CONNECTED,          /* Telnet connected and ready */
    L2_STATE_DISCONNECTING,      /* Telnet disconnection in progress */
    L2_STATE_ERROR              /* Error state requiring reset */
} level2_state_t;

/**
 * Level 2 thread states
 * Used to coordinate telnet thread lifecycle
 */
typedef enum {
    L2_THREAD_IDLE = 0,         /* Thread waiting for work */
    L2_THREAD_RUNNING,          /* Thread actively processing */
    L2_THREAD_STOPPING,         /* Thread shutdown requested */
    L2_THREAD_STOPPED          /* Thread has exited */
} level2_thread_state_t;

/* ========================================================================
 * Level 2 Statistics
 * ========================================================================*/

/**
 * Level 2 performance statistics
 * Tracks telnet-specific metrics
 */
typedef struct {
    /* Data transfer statistics */
    uint64_t bytes_from_telnet;     /* Total bytes received from telnet */
    uint64_t bytes_to_telnet;        /* Total bytes sent to telnet */
    uint64_t packets_from_telnet;   /* Total packets from telnet */
    uint64_t packets_to_telnet;     /* Total packets to telnet */

    /* Connection statistics */
    uint32_t connection_count;       /* Total number of connections */
    uint32_t connection_failures;    /* Failed connection attempts */
    uint32_t disconnection_count;    /* Total disconnections */
    time_t last_connect_time;        /* Last successful connection time */
    time_t last_disconnect_time;     /* Last disconnection time */

    /* Protocol statistics */
    uint32_t iac_commands_received;  /* IAC commands processed */
    uint32_t iac_commands_sent;      /* IAC commands transmitted */
    uint32_t iac_negotiations;       /* Option negotiations completed */

    /* Error statistics */
    uint32_t recv_errors;            /* Receive errors */
    uint32_t send_errors;            /* Send errors */
    uint32_t protocol_errors;        /* Telnet protocol violations */
} level2_stats_t;

/* ========================================================================
 * Level 2 Configuration
 * ========================================================================*/

/**
 * Level 2 runtime configuration
 * Separate from main config for modular operation
 */
typedef struct {
    /* Connection parameters */
    bool auto_reconnect;             /* Auto-reconnect on disconnect */
    int reconnect_delay;             /* Delay between reconnect attempts (ms) */
    int connect_timeout;             /* Connection timeout (ms) */

    /* Protocol options */
    bool binary_mode;                /* Use binary transmission mode */
    bool echo_sync;                  /* Synchronize echo with modem */
    bool suppress_go_ahead;          /* Suppress Go Ahead option */

    /* Performance tuning */
    int recv_buffer_size;            /* Receive buffer size */
    int send_buffer_size;            /* Send buffer size */
    bool nodelay;                    /* TCP_NODELAY option */
} level2_config_t;

/* ========================================================================
 * Level 2 Context
 * ========================================================================*/

/**
 * Level 2 operation context
 * Encapsulates all Level 2 specific state
 */
typedef struct {
    /* State management */
    level2_state_t state;            /* Current connection state */
    level2_thread_state_t thread_state; /* Thread state */

    /* Configuration */
    level2_config_t config;          /* Runtime configuration */

    /* Statistics */
    level2_stats_t stats;            /* Performance statistics */

    /* Thread coordination */
    bool thread_running;             /* Thread active flag */
    bool shutdown_requested;         /* Shutdown request flag */

    /* Connection info */
    char remote_host[256];           /* Connected host */
    int remote_port;                 /* Connected port */
    time_t connect_time;             /* Connection timestamp */
} level2_context_t;

/* ========================================================================
 * Level 2 Events
 * ========================================================================*/

/**
 * Level 2 event types
 * Used for event-driven processing
 */
typedef enum {
    L2_EVENT_NONE = 0,
    L2_EVENT_CONNECT_REQUEST,       /* Connection requested */
    L2_EVENT_CONNECTED,              /* Connection established */
    L2_EVENT_DISCONNECT_REQUEST,     /* Disconnection requested */
    L2_EVENT_DISCONNECTED,           /* Connection lost */
    L2_EVENT_DATA_AVAILABLE,        /* Data ready to process */
    L2_EVENT_ERROR                  /* Error occurred */
} level2_event_type_t;

/**
 * Level 2 event structure
 */
typedef struct {
    level2_event_type_t type;       /* Event type */
    void *data;                      /* Event-specific data */
    size_t data_size;                /* Size of event data */
    time_t timestamp;                /* Event timestamp */
} level2_event_t;

#endif /* ENABLE_LEVEL2 */

#endif /* LEVEL2_TYPES_H */