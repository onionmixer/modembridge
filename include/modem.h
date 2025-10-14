/*
 * modem.h - Modem control and Hayes AT command handling
 *
 * Implements Hayes AT command set for modem emulation and
 * handles modem handshake, connection negotiation, and state management
 */

#ifndef MODEMBRIDGE_MODEM_H
#define MODEMBRIDGE_MODEM_H

#include "common.h"
#include "serial.h"

/* Modem state */
typedef enum {
    MODEM_STATE_COMMAND,        /* In command mode (AT commands) */
    MODEM_STATE_ONLINE,         /* In online mode (data connection) */
    MODEM_STATE_RINGING,        /* Incoming call */
    MODEM_STATE_CONNECTING,     /* Establishing connection */
    MODEM_STATE_DISCONNECTED    /* No connection */
} modem_state_t;

/* Modem settings */
typedef struct {
    bool echo;                  /* Command echo (ATE) */
    bool verbose;               /* Verbose responses (ATV) */
    bool quiet;                 /* Quiet mode (ATQ) */
    int s_registers[256];       /* S-registers (ATS) */
} modem_settings_t;

/* Modem structure */
typedef struct {
    serial_port_t *serial;      /* Serial port handle */
    modem_state_t state;        /* Current modem state */
    modem_settings_t settings;  /* Modem settings */

    char cmd_buffer[LINE_BUFFER_SIZE];  /* Command buffer */
    size_t cmd_len;                     /* Command length */

    bool online;                /* Online status */
    bool carrier;               /* Carrier detect */

    /* Escape sequence detection (+++ATH) */
    int escape_count;           /* Number of '+' received */
    time_t last_escape_time;    /* Time of last escape character */
} modem_t;

/* AT command response codes */
#define MODEM_RESP_OK           "OK"
#define MODEM_RESP_ERROR        "ERROR"
#define MODEM_RESP_CONNECT      "CONNECT"
#define MODEM_RESP_NO_CARRIER   "NO CARRIER"
#define MODEM_RESP_RING         "RING"
#define MODEM_RESP_NO_DIALTONE  "NO DIALTONE"
#define MODEM_RESP_BUSY         "BUSY"
#define MODEM_RESP_NO_ANSWER    "NO ANSWER"

/* S-register definitions */
#define SREG_AUTO_ANSWER        0   /* Auto answer (rings) */
#define SREG_RING_COUNT         1   /* Ring counter */
#define SREG_ESCAPE_CHAR        2   /* Escape character (default '+') */
#define SREG_CR_CHAR            3   /* Carriage return character */
#define SREG_LF_CHAR            4   /* Line feed character */
#define SREG_BS_CHAR            5   /* Backspace character */

/* Function prototypes */

/**
 * Initialize modem structure
 * @param modem Modem structure to initialize
 * @param serial Serial port handle
 */
void modem_init(modem_t *modem, serial_port_t *serial);

/**
 * Reset modem to default settings
 * @param modem Modem structure
 */
void modem_reset(modem_t *modem);

/**
 * Process incoming data from serial port
 * Handles AT commands in command mode and escape sequence in online mode
 * @param modem Modem structure
 * @param data Incoming data
 * @param len Data length
 * @return Number of bytes consumed, or error code on failure
 */
ssize_t modem_process_input(modem_t *modem, const char *data, size_t len);

/**
 * Process AT command
 * @param modem Modem structure
 * @param command Command string (without AT prefix)
 * @return SUCCESS on success, error code on failure
 */
int modem_process_command(modem_t *modem, const char *command);

/**
 * Send response to client
 * @param modem Modem structure
 * @param response Response string
 * @return SUCCESS on success, error code on failure
 */
int modem_send_response(modem_t *modem, const char *response);

/**
 * Send formatted response to client
 * @param modem Modem structure
 * @param format Printf-style format string
 * @param ... Format arguments
 * @return SUCCESS on success, error code on failure
 */
int modem_send_response_fmt(modem_t *modem, const char *format, ...);

/**
 * Send RING notification to client
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_send_ring(modem_t *modem);

/**
 * Send CONNECT notification with baudrate
 * @param modem Modem structure
 * @param baudrate Connection baudrate (for display)
 * @return SUCCESS on success, error code on failure
 */
int modem_send_connect(modem_t *modem, int baudrate);

/**
 * Send NO CARRIER notification
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_send_no_carrier(modem_t *modem);

/**
 * Go online (data mode)
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_go_online(modem_t *modem);

/**
 * Go offline (command mode)
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_go_offline(modem_t *modem);

/**
 * Hang up connection
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_hangup(modem_t *modem);

/**
 * Answer incoming call
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_answer(modem_t *modem);

/**
 * Set carrier state
 * @param modem Modem structure
 * @param state Carrier state (true = on, false = off)
 * @return SUCCESS on success, error code on failure
 */
int modem_set_carrier(modem_t *modem, bool state);

/**
 * Get modem state
 * @param modem Modem structure
 * @return Current modem state
 */
modem_state_t modem_get_state(modem_t *modem);

/**
 * Check if modem is online
 * @param modem Modem structure
 * @return true if online, false otherwise
 */
bool modem_is_online(modem_t *modem);

/**
 * Get S-register value
 * @param modem Modem structure
 * @param reg Register number (0-255)
 * @return Register value
 */
int modem_get_sreg(modem_t *modem, int reg);

/**
 * Set S-register value
 * @param modem Modem structure
 * @param reg Register number (0-255)
 * @param value Value to set
 * @return SUCCESS on success, error code on failure
 */
int modem_set_sreg(modem_t *modem, int reg, int value);

#endif /* MODEMBRIDGE_MODEM_H */
