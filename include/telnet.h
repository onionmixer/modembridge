/*
 * telnet.h - Telnet client protocol implementation
 *
 * Implements Telnet protocol (RFC 854) for connecting to telnet servers
 * Handles IAC commands, option negotiation, line mode, and character mode
 */

#ifndef MODEMBRIDGE_TELNET_H
#define MODEMBRIDGE_TELNET_H

#ifdef ENABLE_LEVEL2

#include "common.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <fcntl.h>

/* Telnet protocol constants (RFC 854) */
#define TELNET_IAC          255     /* Interpret As Command */
#define TELNET_DONT         254     /* Don't use option */
#define TELNET_DO           253     /* Do use option */
#define TELNET_WONT         252     /* Won't use option */
#define TELNET_WILL         251     /* Will use option */
#define TELNET_SB           250     /* Subnegotiation begin */
#define TELNET_GA           249     /* Go ahead */
#define TELNET_EL           248     /* Erase line */
#define TELNET_EC           247     /* Erase character */
#define TELNET_AYT          246     /* Are you there */
#define TELNET_AO           245     /* Abort output */
#define TELNET_IP           244     /* Interrupt process */
#define TELNET_BREAK        243     /* Break */
#define TELNET_DM           242     /* Data mark */
#define TELNET_NOP          241     /* No operation */
#define TELNET_SE           240     /* Subnegotiation end */
#define TELNET_EOR          239     /* End of record */

/* Telnet options */
#define TELOPT_BINARY       0       /* Binary transmission */
#define TELOPT_ECHO         1       /* Echo */
#define TELOPT_SGA          3       /* Suppress go ahead */
#define TELOPT_STATUS       5       /* Status */
#define TELOPT_TIMING_MARK  6       /* Timing mark */
#define TELOPT_TTYPE        24      /* Terminal type */
#define TELOPT_NAWS         31      /* Negotiate about window size */
#define TELOPT_TSPEED       32      /* Terminal speed */
#define TELOPT_LFLOW        33      /* Remote flow control */
#define TELOPT_LINEMODE     34      /* Linemode */
#define TELOPT_ENVIRON      36      /* Environment variables */

/* TERMINAL-TYPE subnegotiation codes (RFC 1091) */
#define TTYPE_IS            0       /* Terminal type IS */
#define TTYPE_SEND          1       /* Send terminal type */

/* LINEMODE subnegotiation codes (RFC 1184) */
#define LM_MODE             1       /* Linemode MODE */
#define LM_FORWARDMASK      2       /* Forward mask */
#define LM_SLC              3       /* Set local characters */

/* LINEMODE MODE bits */
#define MODE_EDIT           0x01    /* Local editing */
#define MODE_TRAPSIG        0x02    /* Trap signals */
#define MODE_ACK            0x04    /* Acknowledge mode change */
#define MODE_SOFT_TAB       0x08    /* Soft tab */
#define MODE_LIT_ECHO       0x10    /* Literal echo */

/* Telnet state machine states */
typedef enum {
    TELNET_STATE_DATA,          /* Normal data */
    TELNET_STATE_IAC,           /* Received IAC */
    TELNET_STATE_WILL,          /* Received WILL */
    TELNET_STATE_WONT,          /* Received WONT */
    TELNET_STATE_DO,            /* Received DO */
    TELNET_STATE_DONT,          /* Received DONT */
    TELNET_STATE_SB,            /* In subnegotiation */
    TELNET_STATE_SB_IAC         /* Received IAC in subnegotiation */
} telnet_state_t;

/* Telnet connection structure */
typedef struct {
    int fd;                         /* Socket file descriptor */
    int epoll_fd;                   /* Epoll file descriptor */
    char host[SMALL_BUFFER_SIZE];   /* Remote host */
    int port;                       /* Remote port */
    bool is_connected;              /* Connection status */
    bool is_connecting;             /* Connection in progress */

    /* Protocol state */
    telnet_state_t state;           /* Current protocol state */
    unsigned char option;           /* Current option being negotiated */

    /* Subnegotiation buffer */
    unsigned char sb_buffer[BUFFER_SIZE];
    size_t sb_len;

    /* Option tracking */
    bool local_options[256];        /* Options we support locally */
    bool remote_options[256];       /* Options remote supports */

    /* Mode flags (bidirectional - RFC 855 compliant) */
    bool binary_local;              /* We send binary */
    bool binary_remote;             /* They send binary */
    bool echo_local;                /* We echo */
    bool echo_remote;               /* They echo */
    bool sga_local;                 /* We suppress GA */
    bool sga_remote;                /* They suppress GA */
    bool linemode_active;           /* Linemode option active */
    bool linemode_edit;             /* Local editing enabled */

    /* Deprecated: kept for compatibility, use bidirectional flags */
    bool binary_mode;               /* Binary transmission mode (OR of local/remote) */
    bool echo_mode;                 /* Echo mode (remote echo) */
    bool sga_mode;                  /* Suppress go ahead (OR of local/remote) */
    bool linemode;                  /* Line mode vs character mode */

    /* Terminal type */
    char terminal_type[64];         /* Terminal type (e.g., "ANSI", "VT100") */

    /* Data logging (optional - opaque pointer) */
    void *datalog;                  /* Data logger pointer (set externally) */

    /* Epoll event management */
    struct epoll_event events[8];   /* Epoll events array */
    int event_count;                /* Number of active events */

    /* Connection state for epoll */
    bool can_read;                  /* Socket is readable */
    bool can_write;                 /* Socket is writable */
    bool has_error;                 /* Socket has error */

    /* Non-blocking I/O buffers */
    unsigned char read_buf[BUFFER_SIZE];  /* Read buffer */
    size_t read_pos;                /* Read buffer position */
    size_t read_len;                /* Read buffer length */

    unsigned char write_buf[BUFFER_SIZE * 2]; /* Write buffer */
    size_t write_pos;               /* Write buffer position */
    size_t write_len;               /* Write buffer length */

    /* Connection health monitoring */
    time_t last_activity;           /* Last data send/receive timestamp */
    time_t last_ping;               /* Last keep-alive ping timestamp */
    int ping_interval;              /* Keep-alive ping interval (seconds) */
    int connection_timeout;         /* Connection timeout (seconds) */
    bool keep_alive_enabled;        /* Keep-alive functionality enabled */

    /* Error handling and recovery */
    int consecutive_errors;         /* Count of consecutive I/O errors */
    int max_consecutive_errors;     /* Maximum errors before reconnect attempt */
    time_t last_error_time;         /* Timestamp of last error */
    bool auto_reconnect;            /* Enable automatic reconnection */
    int reconnect_interval;         /* Seconds to wait before reconnect */
} telnet_t;

/* Function prototypes */

/**
 * Initialize telnet structure
 * @param tn Telnet structure to initialize
 */
void telnet_init(telnet_t *tn);

/**
 * Connect to telnet server
 * @param tn Telnet structure
 * @param host Remote host (IP or hostname)
 * @param port Remote port
 * @return SUCCESS on success, error code on failure
 */
int telnet_connect(telnet_t *tn, const char *host, int port);

/**
 * Disconnect from telnet server
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_disconnect(telnet_t *tn);

/**
 * Process incoming data from telnet server
 * Handles IAC sequences and returns clean data
 * @param tn Telnet structure
 * @param input Input data buffer
 * @param input_len Input data length
 * @param output Output buffer for clean data
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, error code on failure
 */
int telnet_process_input(telnet_t *tn, const unsigned char *input, size_t input_len,
                         unsigned char *output, size_t output_size, size_t *output_len);

/**
 * Prepare data for sending to telnet server
 * Escapes IAC bytes (0xFF -> 0xFF 0xFF)
 * @param tn Telnet structure
 * @param input Input data buffer
 * @param input_len Input data length
 * @param output Output buffer for escaped data
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, error code on failure
 */
int telnet_prepare_output(telnet_t *tn, const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_size, size_t *output_len);

/**
 * Send data to telnet server
 * @param tn Telnet structure
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or error code on failure
 */
ssize_t telnet_send(telnet_t *tn, const void *data, size_t len);

/**
 * Receive data from telnet server
 * @param tn Telnet structure
 * @param buffer Buffer to store received data
 * @param size Maximum bytes to receive
 * @return Number of bytes received, or error code on failure
 */
ssize_t telnet_recv(telnet_t *tn, void *buffer, size_t size);

/**
 * Send IAC command
 * @param tn Telnet structure
 * @param command Command byte
 * @return SUCCESS on success, error code on failure
 */
int telnet_send_command(telnet_t *tn, unsigned char command);

/**
 * Send option negotiation
 * @param tn Telnet structure
 * @param command WILL/WONT/DO/DONT
 * @param option Option code
 * @return SUCCESS on success, error code on failure
 */
int telnet_send_negotiate(telnet_t *tn, unsigned char command, unsigned char option);

/**
 * Handle received option negotiation
 * @param tn Telnet structure
 * @param command WILL/WONT/DO/DONT
 * @param option Option code
 * @return SUCCESS on success, error code on failure
 */
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option);

/**
 * Handle subnegotiation
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_handle_subnegotiation(telnet_t *tn);

/**
 * Get file descriptor for select/poll
 * @param tn Telnet structure
 * @return File descriptor, or -1 if not connected
 */
int telnet_get_fd(telnet_t *tn);

/**
 * Check if connected to telnet server
 * @param tn Telnet structure
 * @return true if connected, false otherwise
 */
bool telnet_is_connected(telnet_t *tn);

/**
 * Check if in line mode
 * @param tn Telnet structure
 * @return true if line mode, false if character mode
 */
bool telnet_is_linemode(telnet_t *tn);

/**
 * Check if in binary mode
 * @param tn Telnet structure
 * @return true if binary mode, false otherwise
 */
bool telnet_is_binary_mode(telnet_t *tn);

/* Epoll-based network functions */

/**
 * Initialize epoll for telnet connection
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_init_epoll(telnet_t *tn);

/**
 * Process epoll events for telnet connection
 * @param tn Telnet structure
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking)
 * @return SUCCESS on success, error code on failure
 */
int telnet_process_events(telnet_t *tn, int timeout_ms);

/**
 * Check if telnet connection is ready for reading
 * @param tn Telnet structure
 * @return true if readable, false otherwise
 */
bool telnet_can_read(telnet_t *tn);

/**
 * Check if telnet connection is ready for writing
 * @param tn Telnet structure
 * @return true if writable, false otherwise
 */
bool telnet_can_write(telnet_t *tn);

/**
 * Check if telnet connection has error
 * @param tn Telnet structure
 * @return true if error, false otherwise
 */
bool telnet_has_error(telnet_t *tn);

/**
 * Queue data for writing to telnet connection (non-blocking)
 * @param tn Telnet structure
 * @param data Data to write
 * @param len Data length
 * @return SUCCESS on success, error code on failure
 */
int telnet_queue_write(telnet_t *tn, const void *data, size_t len);

/**
 * Process pending write data for telnet connection
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_flush_writes(telnet_t *tn);

/**
 * Process pending read data for telnet connection
 * @param tn Telnet structure
 * @param output Output buffer for clean data
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, error code on failure
 */
int telnet_process_reads(telnet_t *tn, unsigned char *output, size_t output_size, size_t *output_len);

/**
 * Check connection health and send keep-alive if needed
 * @param tn Telnet structure
 * @return SUCCESS on success, error code on failure
 */
int telnet_check_connection_health(telnet_t *tn);

/**
 * Update activity timestamp (called on send/receive)
 * @param tn Telnet structure
 */
void telnet_update_activity(telnet_t *tn);

/**
 * Enable/disable keep-alive functionality
 * @param tn Telnet structure
 * @param enabled true to enable keep-alive
 * @param ping_interval Ping interval in seconds
 * @param connection_timeout Connection timeout in seconds
 */
void telnet_set_keepalive(telnet_t *tn, bool enabled, int ping_interval, int connection_timeout);

/**
 * Handle telnet I/O error with automatic recovery
 * @param tn Telnet structure
 * @param error_code Error code that occurred
 * @param operation Description of operation that failed ("read", "write", etc.)
 * @return SUCCESS if error handled/recovered, error code if unrecoverable
 */
int telnet_handle_error(telnet_t *tn, int error_code, const char *operation);

/**
 * Configure error handling and auto-reconnect settings
 * @param tn Telnet structure
 * @param auto_reconnect Enable automatic reconnection
 * @param max_consecutive_errors Maximum errors before reconnect attempt
 * @param reconnect_interval Seconds to wait before reconnect
 */
void telnet_set_error_handling(telnet_t *tn, bool auto_reconnect, int max_consecutive_errors, int reconnect_interval);

/**
 * Reset error counters after successful operation
 * @param tn Telnet structure
 */
void telnet_reset_error_state(telnet_t *tn);

/**
 * Check if reconnection should be attempted
 * @param tn Telnet structure
 * @return true if reconnection should be attempted, false otherwise
 */
bool telnet_should_reconnect(telnet_t *tn);

#endif /* ENABLE_LEVEL2 */

#endif /* MODEMBRIDGE_TELNET_H */
