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

    /* Extended AT command settings */
    int dcd_mode;               /* AT&C: DCD control (0=always ON, 1=follow carrier) */
    int dtr_mode;               /* AT&D: DTR handling (0=ignore, 1=command, 2=hangup, 3=reset) */
    int bell_mode;              /* ATB: Bell/CCITT (0=CCITT, 1=Bell) */
    int result_mode;            /* ATX: Extended result codes (0-4) */
    int speaker_volume;         /* ATL: Speaker volume (0-3) */
    int speaker_control;        /* ATM: Speaker control (0-3) */
    int error_correction;       /* AT\\N: Error correction (0-3) */
    int dsr_mode;               /* AT&S: DSR override (0-1) */

    /* Configuration storage */
    bool profile_saved[2];      /* Profile 0, 1 saved status (AT&W) */
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

    /* Hardware modem message buffer (for handling split messages) */
    char hw_msg_buffer[LINE_BUFFER_SIZE];  /* Buffer for accumulating hardware messages */
    size_t hw_msg_len;                     /* Current length of buffered data */
    time_t hw_msg_last_time;               /* Last time data was added to buffer */

    /* DCD monitoring fields */
    bool dcd_monitoring_enabled;           /* DCD monitoring enabled flag */
    bool last_dcd_state;                   /* Last known DCD state */
    time_t last_dcd_check_time;            /* Last time DCD was checked */

    /* DCD event callback for bridge integration */
    int (*dcd_event_callback)(void *user_data, bool dcd_state);  /* DCD event callback function */
    void *dcd_callback_user_data;          /* User data for DCD callback (bridge context) */
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
#define SREG_ESCAPE_GUARD_TIME  12  /* Escape sequence guard time (in 50ms units) */
#define SREG_ESCAPE_CODE        43  /* Escape character code (default '+') */

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
 * Monitor DCD signal and update modem state accordingly
 * @param modem Modem structure
 * @return SUCCESS on state change, error code on failure
 */
int modem_monitor_dcd_signal(modem_t *modem);

/**
 * Enable/disable DCD-based state transitions
 * @param modem Modem structure
 * @param enabled Enable DCD monitoring (true = enabled, false = disabled)
 * @return SUCCESS on success, error code on failure
 */
int modem_set_dcd_monitoring(modem_t *modem, bool enabled);

/**
 * Check if DCD monitoring is enabled
 * @param modem Modem structure
 * @return true if DCD monitoring is enabled
 */
bool modem_is_dcd_monitoring_enabled(modem_t *modem);

/**
 * Process DCD state change (DCD rising/falling edge detection)
 * @param modem Modem structure
 * @param dcd_state Current DCD signal state (true = high, false = low)
 * @return SUCCESS on success, error code on failure
 */
int modem_process_dcd_change(modem_t *modem, bool dcd_state);

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

/**
 * Show current modem configuration (AT&V)
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_show_configuration(modem_t *modem);

/**
 * Process unsolicited message from hardware modem (RING, CONNECT, etc.)
 * This handles responses from a REAL hardware modem, not commands FROM the client
 * @param modem Modem structure
 * @param data Incoming data from hardware modem
 * @param len Data length
 * @return true if hardware message detected (RING/CONNECT/NO CARRIER), false otherwise
 */
bool modem_process_hardware_message(modem_t *modem, const char *data, size_t len);

/* ===== Extended functions from modem_sample ===== */

/**
 * Send AT command and wait for response (synchronous)
 * Based on modem_sample/modem_control.c:send_at_command()
 * Features:
 * - Flushes input before sending
 * - Waits for response using serial_read_line()
 * - Detects OK/ERROR/CONNECT/NO CARRIER automatically
 * - Stores response in buffer
 * @param modem Modem structure
 * @param command Command string (e.g., "ATZ" or "AT&F")
 * @param response Buffer to store response (can be NULL)
 * @param resp_size Size of response buffer
 * @param timeout_sec Timeout in seconds
 * @return SUCCESS on OK, ERROR_MODEM on ERROR/NO CARRIER, ERROR_TIMEOUT on timeout
 */
int modem_send_at_command(modem_t *modem, const char *command,
                          char *response, size_t resp_size, int timeout_sec);

/**
 * Send compound AT command string (semicolon-separated)
 * Based on modem_sample/modem_control.c:send_command_string()
 * Example: "ATZ; AT&F Q0 V1 X4"
 * @param modem Modem structure
 * @param cmd_string Compound command string
 * @param timeout_sec Timeout per command
 * @return SUCCESS on success, error code on failure
 */
int modem_send_command_string(modem_t *modem, const char *cmd_string, int timeout_sec);

/**
 * Parse connection speed from CONNECT response
 * Based on modem_sample/modem_control.c:parse_connect_speed()
 * Examples:
 * - "CONNECT 1200" -> 1200
 * - "CONNECT 2400/ARQ" -> 2400
 * - "CONNECT" -> 300 (default)
 * @param connect_str CONNECT response string
 * @return Connection speed in bps, or -1 on error
 */
int modem_parse_connect_speed(const char *connect_str);

/**
 * Wait for RING signal and optional connection
 * Based on modem_sample/modem_sample.c:wait_for_ring()
 * Modes:
 * - SOFTWARE (S0=0): Wait for 2 RINGs, caller must send ATA
 * - HARDWARE (S0>0): Wait for RING, modem auto-answers, wait for CONNECT
 * @param modem Modem structure
 * @param timeout_sec Total timeout in seconds
 * @param connected_speed Pointer to store connection speed (can be NULL)
 * @return SUCCESS on RING or CONNECT, error code on failure
 */
int modem_wait_for_ring(modem_t *modem, int timeout_sec, int *connected_speed);

/**
 * Answer incoming call (send ATA and wait for CONNECT)
 * Based on modem_sample/modem_control.c:modem_answer_with_speed_adjust()
 * Used in SOFTWARE mode (S0=0) after detecting RINGs
 * @param modem Modem structure
 * @param connected_speed Pointer to store connection speed (can be NULL)
 * @return SUCCESS on CONNECT, error code on failure
 */
int modem_answer_call(modem_t *modem, int *connected_speed);

/**
 * Convert baudrate integer to speed_t
 * @param baudrate Baudrate as integer (e.g., 9600)
 * @return speed_t constant (e.g., B9600), or B9600 on error
 */
speed_t modem_baudrate_to_speed_t(int baudrate);

/**
 * Convert modem state to string for logging
 * @param state Modem state
 * @return String representation of state
 */
const char *modem_state_to_string(modem_state_t state);

/* ===== Level 1 Hayes Command Filtering Functions ===== */

/**
 * Check if data should be filtered based on current mode and Hayes settings
 * @param modem Modem structure
 * @param data Incoming data buffer
 * @param len Data length
 * @param is_command_mode True if in command mode, False if in data mode
 * @return Number of bytes that should be filtered/consumed
 */
size_t modem_filter_hayes_data(modem_t *modem, const char *data, size_t len, bool is_command_mode);

/**
 * Handle Hayes escape sequence detection with proper S2/S12 register support
 * @param modem Modem structure
 * @param data Incoming data buffer
 * @param len Data length
 * @param consumed Pointer to store number of bytes consumed
 * @return true if escape sequence detected, false otherwise
 */
bool modem_check_escape_sequence(modem_t *modem, const char *data, size_t len, size_t *consumed);

/**
 * Get escape guard time in milliseconds from S12 register
 * @param modem Modem structure
 * @return Guard time in milliseconds
 */
int modem_get_escape_guard_time(modem_t *modem);

/**
 * Get escape character from S2 register
 * @param modem Modem structure
 * @return Escape character code
 */
char modem_get_escape_character(modem_t *modem);

/**
 * Process command mode echo control with enhanced ATE handling
 * @param modem Modem structure
 * @param data Data to echo
 * @param len Data length
 * @return SUCCESS on success, error code on failure
 */
int modem_handle_command_echo(modem_t *modem, const char *data, size_t len);

/**
 * Enhanced result code filtering based on Q, V, X register settings
 * @param modem Modem structure
 * @param response Original response string
 * @param filtered_response Buffer to store filtered response
 * @param resp_size Size of filtered response buffer
 * @return SUCCESS on success, error code on failure
 */
int modem_filter_result_code(modem_t *modem, const char *response,
                           char *filtered_response, size_t resp_size);

/* ===== Level 1 DTR/DCD Coordination Functions ===== */

/**
 * Handle DTR signal changes with normalized Hayes &D settings
 * @param modem Modem structure
 * @param dtr_state Current DTR signal state (true = high, false = low)
 * @return SUCCESS on success, error code on failure
 */
int modem_handle_dtr_change(modem_t *modem, bool dtr_state);

/**
 * Process immediate data mode termination on NO CARRIER receipt
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_handle_no_carrier_termination(modem_t *modem);

/**
 * Process immediate cleanup transition on DCD falling edge
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_handle_dcd_falling_cleanup(modem_t *modem);

/**
 * Enhanced modem_go_offline with DTR/DCD coordination
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_go_offline_enhanced(modem_t *modem);

/**
 * Check and process pending DTR/DCD state transitions
 * @param modem Modem structure
 * @return SUCCESS on success, error code on failure
 */
int modem_process_dtr_dcd_transitions(modem_t *modem);

/**
 * Set DCD event callback for bridge integration
 * @param modem Modem structure
 * @param callback Callback function to call on DCD events (can be NULL)
 * @param user_data User data to pass to callback (typically bridge context)
 * @return SUCCESS on success, error code on failure
 */
int modem_set_dcd_event_callback(modem_t *modem,
                                int (*callback)(void *user_data, bool dcd_state),
                                void *user_data);

#endif /* MODEMBRIDGE_MODEM_H */
