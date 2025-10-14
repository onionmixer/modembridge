/*
 * telnet.h - Telnet client protocol implementation
 *
 * Implements Telnet protocol (RFC 854) for connecting to telnet servers
 * Handles IAC commands, option negotiation, line mode, and character mode
 */

#ifndef MODEMBRIDGE_TELNET_H
#define MODEMBRIDGE_TELNET_H

#include "common.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

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
    char host[SMALL_BUFFER_SIZE];   /* Remote host */
    int port;                       /* Remote port */
    bool is_connected;              /* Connection status */

    /* Protocol state */
    telnet_state_t state;           /* Current protocol state */
    unsigned char option;           /* Current option being negotiated */

    /* Subnegotiation buffer */
    unsigned char sb_buffer[BUFFER_SIZE];
    size_t sb_len;

    /* Option tracking */
    bool local_options[256];        /* Options we support locally */
    bool remote_options[256];       /* Options remote supports */

    /* Mode flags */
    bool binary_mode;               /* Binary transmission mode */
    bool echo_mode;                 /* Echo mode */
    bool sga_mode;                  /* Suppress go ahead */
    bool linemode;                  /* Line mode vs character mode */
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

#endif /* MODEMBRIDGE_TELNET_H */
