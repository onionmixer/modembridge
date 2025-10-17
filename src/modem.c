/*
 * modem.c - Modem control and Hayes AT command implementation
 */

#include "modem.h"
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

/* Escape sequence timing (milliseconds) */
#define ESCAPE_GUARD_TIME 1000

/**
 * Initialize modem structure
 */
void modem_init(modem_t *modem, serial_port_t *serial)
{
    if (modem == NULL) {
        return;
    }

    memset(modem, 0, sizeof(modem_t));
    modem->serial = serial;
    modem->state = MODEM_STATE_COMMAND;
    modem->online = false;
    modem->carrier = false;

    /* Initialize settings with defaults */
    modem_reset(modem);

    MB_LOG_DEBUG("Modem initialized");
}

/**
 * Reset modem to default settings
 */
void modem_reset(modem_t *modem)
{
    if (modem == NULL) {
        return;
    }

    MB_LOG_INFO("Resetting modem to defaults");

    /* Reset settings */
    modem->settings.echo = true;
    modem->settings.verbose = true;
    modem->settings.quiet = false;

    /* Initialize S-registers with default values */
    memset(modem->settings.s_registers, 0, sizeof(modem->settings.s_registers));
    /* modem->settings.s_registers[SREG_AUTO_ANSWER] = 0; */  /* Configure via MODEM_AUTOANSWER_COMMAND */
    modem->settings.s_registers[SREG_RING_COUNT] = 0;       /* Ring counter */
    modem->settings.s_registers[SREG_ESCAPE_CHAR] = '+';    /* Escape character */
    modem->settings.s_registers[SREG_CR_CHAR] = '\r';       /* CR character */
    modem->settings.s_registers[SREG_LF_CHAR] = '\n';       /* LF character */
    modem->settings.s_registers[SREG_BS_CHAR] = '\b';       /* Backspace character */
    modem->settings.s_registers[SREG_ESCAPE_GUARD_TIME] = 50; /* 50 * 50ms = 2.5 seconds default guard time */
    modem->settings.s_registers[SREG_ESCAPE_CODE] = '+';      /* Escape character code */

    /* Initialize extended AT command settings */
    modem->settings.dcd_mode = 1;           /* &C1: DCD follows carrier */
    modem->settings.dtr_mode = 2;           /* &D2: DTR OFF = hangup */
    modem->settings.bell_mode = 0;          /* B0: CCITT */
    modem->settings.result_mode = 4;        /* X4: All extended codes */
    modem->settings.speaker_volume = 2;     /* L2: Medium */
    modem->settings.speaker_control = 1;    /* M1: On until carrier */
    modem->settings.error_correction = 3;   /* \N3: Auto reliable */
    modem->settings.dsr_mode = 0;           /* &S0: DSR always on */

    /* Clear profile saved flags */
    memset(modem->settings.profile_saved, 0, sizeof(modem->settings.profile_saved));

    /* Clear command buffer */
    memset(modem->cmd_buffer, 0, sizeof(modem->cmd_buffer));
    modem->cmd_len = 0;

    /* Reset escape sequence detection */
    modem->escape_count = 0;
    modem->last_escape_time = 0;

    /* Reset hardware message buffer */
    memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
    modem->hw_msg_len = 0;
    modem->hw_msg_last_time = 0;

    /* Initialize DCD monitoring */
    modem->dcd_monitoring_enabled = false;
    modem->last_dcd_state = false;
    modem->last_dcd_check_time = 0;

    /* Initialize DCD callback */
    modem->dcd_event_callback = NULL;
    modem->dcd_callback_user_data = NULL;

    /* Set state */
    modem->state = MODEM_STATE_COMMAND;
    modem->online = false;

    MB_LOG_DEBUG("Modem reset complete");
}

/**
 * Send response to client
 */
int modem_send_response(modem_t *modem, const char *response)
{
    char buf[SMALL_BUFFER_SIZE];
    char filtered_response[SMALL_BUFFER_SIZE];
    int len;

    if (modem == NULL || response == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Apply enhanced result code filtering based on Q, V, X settings */
    int rc = modem_filter_result_code(modem, response, filtered_response, sizeof(filtered_response));
    if (rc != SUCCESS) {
        MB_LOG_ERROR("Failed to filter response");
        return rc;
    }

    /* Check if response was filtered out (quiet mode) */
    if (filtered_response[0] == '\0') {
        return SUCCESS;  /* Quiet mode - no response */
    }

    /* Format response based on verbose/numeric mode */
    if (modem->settings.verbose) {
        /* Verbose mode: \r\nRESPONSE\r\n */
        len = snprintf(buf, sizeof(buf), "\r\n%s\r\n", filtered_response);
    } else {
        /* Numeric mode responses are already handled in filtering */
        len = snprintf(buf, sizeof(buf), "\r\n%s\r\n", filtered_response);
    }

    if (len >= (int)sizeof(buf)) {
        MB_LOG_WARNING("Response truncated");
        len = sizeof(buf) - 1;
    }

    MB_LOG_DEBUG("Modem response: %s -> %s", response, filtered_response);

    /* Send response via serial port */
    ssize_t sent = serial_write(modem->serial, buf, len);
    if (sent < 0) {
        MB_LOG_ERROR("Failed to send modem response");
        return ERROR_IO;
    }

    return SUCCESS;
}

/**
 * Send formatted response to client
 */
int modem_send_response_fmt(modem_t *modem, const char *format, ...)
{
    char response[SMALL_BUFFER_SIZE];
    va_list args;

    if (modem == NULL || format == NULL) {
        return ERROR_INVALID_ARG;
    }

    va_list args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    vsnprintf(response, sizeof(response), format, args_copy);
    va_end(args_copy);
    va_end(args);

    return modem_send_response(modem, response);
}

/**
 * Send RING notification to client
 */
int modem_send_ring(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Sending RING");
    modem->settings.s_registers[SREG_RING_COUNT]++;

    return modem_send_response(modem, MODEM_RESP_RING);
}

/**
 * Send CONNECT notification with baudrate
 */
int modem_send_connect(modem_t *modem, int baudrate)
{
    char response[SMALL_BUFFER_SIZE];

    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Sending CONNECT %d", baudrate);

    if (baudrate > 0) {
        snprintf(response, sizeof(response), "CONNECT %d", baudrate);
    } else {
        snprintf(response, sizeof(response), "CONNECT");
    }

    return modem_send_response(modem, response);
}

/**
 * Send NO CARRIER notification
 */
int modem_send_no_carrier(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Sending NO CARRIER");
    return modem_send_response(modem, MODEM_RESP_NO_CARRIER);
}

/**
 * Go online (data mode)
 */
int modem_go_online(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Going online (data mode)");

    modem->state = MODEM_STATE_ONLINE;
    modem->online = true;
    modem->carrier = true;
    modem->escape_count = 0;
    modem->last_escape_time = 0;

    /* Set DTR and RTS */
    serial_set_dtr(modem->serial, true);
    serial_set_rts(modem->serial, true);

    return SUCCESS;
}

/**
 * Go offline (command mode)
 */
int modem_go_offline(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Going offline (command mode)");

    modem->state = MODEM_STATE_COMMAND;
    modem->online = false;
    modem->escape_count = 0;
    modem->last_escape_time = 0;

    /* Clear command buffer */
    modem->cmd_len = 0;

    return SUCCESS;
}

/**
 * Hang up connection
 * Improved version based on modem_sample/modem_control.c:modem_hangup()
 */
int modem_hangup(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Hanging up modem connection...");

    /* Update state first */
    modem->state = MODEM_STATE_DISCONNECTED;
    modem->online = false;
    modem->carrier = false;

    /* Reset ring counter */
    modem->settings.s_registers[SREG_RING_COUNT] = 0;

    /* If serial port is available, perform proper hangup sequence */
    if (modem->serial && serial_is_open(modem->serial)) {
        /* Flush buffers */
        serial_flush(modem->serial, TCIFLUSH);
        serial_flush(modem->serial, TCOFLUSH);

        /* Small delay before hangup command */
        usleep(500000); /* 500ms */

        /* Disable carrier detect before hangup to prevent I/O errors */
        MB_LOG_DEBUG("Disabling carrier detect for hangup...");
        serial_disable_carrier_detect(modem->serial);

        /* Send ATH hangup command (may timeout if connection already dropped) */
        char response[SMALL_BUFFER_SIZE];
        int rc = modem_send_at_command(modem, "ATH", response, sizeof(response), 3);

        if (rc == SUCCESS) {
            MB_LOG_INFO("ATH command successful");
        } else if (rc == ERROR_TIMEOUT) {
            MB_LOG_INFO("ATH timeout (connection may already be dropped)");
        } else {
            MB_LOG_INFO("ATH command completed (status: %d)", rc);
        }

        /* Perform DTR drop for hardware hangup */
        /* Note: This may fail if carrier is already lost, but that's OK */
        rc = serial_dtr_drop_hangup(modem->serial);
        if (rc != SUCCESS) {
            MB_LOG_INFO("DTR drop completed with warning (connection may already be dropped)");
        }

        /* Flush again */
        serial_flush(modem->serial, TCIFLUSH);
        serial_flush(modem->serial, TCOFLUSH);

        MB_LOG_INFO("Modem hangup completed");
    } else {
        MB_LOG_WARNING("Serial port not available for hangup sequence");
    }

    return SUCCESS; /* Always return success for hangup */
}

/**
 * Answer incoming call
 */
int modem_answer(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Answering call");

    modem->state = MODEM_STATE_CONNECTING;
    modem->settings.s_registers[SREG_RING_COUNT] = 0;

    return SUCCESS;
}

/**
 * Set carrier state
 */
int modem_set_carrier(modem_t *modem, bool state)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Setting carrier to %s", state ? "ON" : "OFF");

    modem->carrier = state;

    return SUCCESS;
}

/**
 * Get modem state
 */
modem_state_t modem_get_state(modem_t *modem)
{
    if (modem == NULL) {
        return MODEM_STATE_DISCONNECTED;
    }

    return modem->state;
}

/**
 * Check if modem is online
 */
bool modem_is_online(modem_t *modem)
{
    if (modem == NULL) {
        return false;
    }

    return modem->online;
}

/**
 * Get S-register value
 */
int modem_get_sreg(modem_t *modem, int reg)
{
    if (modem == NULL || reg < 0 || reg > 255) {
        return 0;
    }

    return modem->settings.s_registers[reg];
}

/**
 * Set S-register value
 */
int modem_set_sreg(modem_t *modem, int reg, int value)
{
    if (modem == NULL || reg < 0 || reg > 255) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Setting S%d=%d", reg, value);
    modem->settings.s_registers[reg] = value;

    return SUCCESS;
}

/**
 * Process AT command
 */
int modem_process_command(modem_t *modem, const char *command)
{
    char cmd_upper[LINE_BUFFER_SIZE];
    const char *p;
    int ret = SUCCESS;

    if (modem == NULL || command == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Convert to uppercase for easier parsing */
    for (size_t i = 0; i < sizeof(cmd_upper) - 1 && command[i]; i++) {
        cmd_upper[i] = toupper((unsigned char)command[i]);
        cmd_upper[i + 1] = '\0';
    }

    MB_LOG_DEBUG("Processing AT command: AT%s", cmd_upper);

    p = cmd_upper;

    /* Handle empty command */
    if (*p == '\0') {
        return modem_send_response(modem, MODEM_RESP_OK);
    }

    /* Parse command(s) - AT commands can be chained */
    while (*p != '\0') {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0') break;

        /* ATA - Answer */
        if (*p == 'A' && *(p + 1) != 'T') {
            MB_LOG_INFO("AT command: ATA (Answer)");
            ret = modem_answer(modem);
            /* ATA command sends OK first, then connection will be handled by bridge */
            p++;
        }
        /* ATB - Bell/CCITT mode */
        else if (*p == 'B') {
            p++;
            int val = 0;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            if (val >= 0 && val <= 1) {
                modem->settings.bell_mode = val;
                MB_LOG_INFO("AT command: ATB%d (Bell mode %s)",
                           val, val ? "Bell 212A" : "CCITT");
            } else {
                return modem_send_response(modem, MODEM_RESP_ERROR);
            }
        }
        /* ATD - Dial (not implemented, just return OK) */
        else if (*p == 'D') {
            MB_LOG_INFO("AT command: ATD (Dial) - not implemented");
            p++;
            /* Skip dial string */
            while (*p && *p != ' ' && *p != ';') p++;
            return modem_send_response(modem, MODEM_RESP_OK);
        }
        /* ATE - Echo */
        else if (*p == 'E') {
            p++;
            int val = 1;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            modem->settings.echo = (val != 0);
            MB_LOG_INFO("AT command: ATE%d (Echo %s)", val, modem->settings.echo ? "ON" : "OFF");
        }
        /* ATH - Hang up */
        else if (*p == 'H') {
            MB_LOG_INFO("AT command: ATH (Hang up)");
            p++;
            /* Skip optional parameter */
            if (isdigit(*p)) p++;
            ret = modem_hangup(modem);
            if (ret == SUCCESS) {
                return modem_send_response(modem, MODEM_RESP_OK);
            }
        }
        /* ATI - Information */
        else if (*p == 'I') {
            p++;
            int val = 0;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            MB_LOG_INFO("AT command: ATI%d (Information)", val);
            /* Return product information */
            modem_send_response_fmt(modem, "ModemBridge v%s", MODEMBRIDGE_VERSION);
            return modem_send_response(modem, MODEM_RESP_OK);
        }
        /* ATL - Speaker volume */
        else if (*p == 'L') {
            p++;
            int val = 2;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            if (val >= 0 && val <= 3) {
                modem->settings.speaker_volume = val;
                MB_LOG_INFO("AT command: ATL%d (Speaker volume)", val);
            } else {
                return modem_send_response(modem, MODEM_RESP_ERROR);
            }
        }
        /* ATM - Speaker control */
        else if (*p == 'M') {
            p++;
            int val = 1;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            if (val >= 0 && val <= 3) {
                modem->settings.speaker_control = val;
                MB_LOG_INFO("AT command: ATM%d (Speaker control)", val);
            } else {
                return modem_send_response(modem, MODEM_RESP_ERROR);
            }
        }
        /* ATO - Return to online mode */
        else if (*p == 'O') {
            MB_LOG_INFO("AT command: ATO (Online)");
            p++;
            /* Skip optional parameter */
            if (isdigit(*p)) p++;
            if (modem->carrier) {
                ret = modem_go_online(modem);
                if (ret == SUCCESS) {
                    return modem_send_connect(modem, 0);
                }
            } else {
                return modem_send_response(modem, MODEM_RESP_NO_CARRIER);
            }
        }
        /* ATQ - Quiet mode */
        else if (*p == 'Q') {
            p++;
            int val = 0;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            modem->settings.quiet = (val != 0);
            MB_LOG_INFO("AT command: ATQ%d (Quiet %s)", val, modem->settings.quiet ? "ON" : "OFF");
        }
        /* ATS - S-register */
        else if (*p == 'S') {
            p++;
            int reg = 0;
            /* Parse register number */
            while (isdigit(*p)) {
                reg = reg * 10 + (*p - '0');
                p++;
            }
            /* Check for = (set) or ? (query) */
            if (*p == '=') {
                p++;
                int val = 0;
                while (isdigit(*p)) {
                    val = val * 10 + (*p - '0');
                    p++;
                }
                MB_LOG_INFO("AT command: ATS%d=%d", reg, val);
                modem_set_sreg(modem, reg, val);
            } else if (*p == '?') {
                p++;
                int val = modem_get_sreg(modem, reg);
                MB_LOG_INFO("AT command: ATS%d? = %d", reg, val);
                modem_send_response_fmt(modem, "%d", val);
                return modem_send_response(modem, MODEM_RESP_OK);
            }
        }
        /* ATV - Verbose mode */
        else if (*p == 'V') {
            p++;
            int val = 1;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            modem->settings.verbose = (val != 0);
            MB_LOG_INFO("AT command: ATV%d (Verbose %s)", val, modem->settings.verbose ? "ON" : "OFF");
        }
        /* ATX - Extended result codes */
        else if (*p == 'X') {
            p++;
            int val = 4;
            if (isdigit(*p)) {
                val = *p - '0';
                p++;
            }
            if (val >= 0 && val <= 4) {
                modem->settings.result_mode = val;
                MB_LOG_INFO("AT command: ATX%d (Result code mode)", val);
            } else {
                return modem_send_response(modem, MODEM_RESP_ERROR);
            }
        }
        /* ATZ - Reset */
        else if (*p == 'Z') {
            MB_LOG_INFO("AT command: ATZ (Reset)");
            p++;
            /* Skip optional profile number */
            if (isdigit(*p)) p++;
            modem_reset(modem);
            return modem_send_response(modem, MODEM_RESP_OK);
        }
        /* AT& commands */
        else if (*p == '&') {
            p++;
            /* AT&C - DCD control */
            if (*p == 'C') {
                p++;
                int val = 1;  /* Default &C1 */
                if (isdigit(*p)) {
                    val = *p - '0';
                    p++;
                }
                if (val >= 0 && val <= 1) {
                    modem->settings.dcd_mode = val;
                    MB_LOG_INFO("AT command: AT&C%d (DCD mode)", val);
                } else {
                    return modem_send_response(modem, MODEM_RESP_ERROR);
                }
            }
            /* AT&D - DTR control */
            else if (*p == 'D') {
                p++;
                int val = 2;  /* Default &D2 */
                if (isdigit(*p)) {
                    val = *p - '0';
                    p++;
                }
                if (val >= 0 && val <= 3) {
                    modem->settings.dtr_mode = val;
                    MB_LOG_INFO("AT command: AT&D%d (DTR mode)", val);
                } else {
                    return modem_send_response(modem, MODEM_RESP_ERROR);
                }
            }
            /* AT&F - Factory defaults */
            else if (*p == 'F') {
                MB_LOG_INFO("AT command: AT&F (Factory defaults)");
                p++;
                modem_reset(modem);
            }
            /* AT&V - View configuration */
            else if (*p == 'V') {
                p++;
                MB_LOG_INFO("AT command: AT&V (View configuration)");
                return modem_show_configuration(modem);
            }
            /* AT&W - Write configuration */
            else if (*p == 'W') {
                p++;
                int profile = 0;
                if (isdigit(*p)) {
                    profile = *p - '0';
                    p++;
                }
                if (profile >= 0 && profile <= 1) {
                    modem->settings.profile_saved[profile] = true;
                    MB_LOG_INFO("AT command: AT&W%d (Save profile)", profile);
                } else {
                    return modem_send_response(modem, MODEM_RESP_ERROR);
                }
            }
            /* AT&S - DSR override */
            else if (*p == 'S') {
                p++;
                int val = 0;
                if (isdigit(*p)) {
                    val = *p - '0';
                    p++;
                }
                if (val >= 0 && val <= 1) {
                    modem->settings.dsr_mode = val;
                    MB_LOG_INFO("AT command: AT&S%d (DSR mode)", val);
                } else {
                    return modem_send_response(modem, MODEM_RESP_ERROR);
                }
            }
            else {
                /* Skip unsupported & command */
                p++;
                if (isdigit(*p)) p++;
            }
        }
        /* AT\ commands */
        else if (*p == '\\') {
            p++;
            /* AT\N - Error correction */
            if (*p == 'N') {
                p++;
                int val = 3;
                if (isdigit(*p)) {
                    val = *p - '0';
                    p++;
                }
                if (val >= 0 && val <= 3) {
                    modem->settings.error_correction = val;
                    MB_LOG_INFO("AT command: AT\\N%d (Error correction)", val);
                } else {
                    return modem_send_response(modem, MODEM_RESP_ERROR);
                }
            }
            else {
                /* Unknown \ command */
                p++;
                if (isdigit(*p)) p++;
            }
        }
        /* Unknown command - skip */
        else {
            MB_LOG_WARNING("Unknown AT command character: %c", *p);
            p++;
        }
    }

    return modem_send_response(modem, MODEM_RESP_OK);
}

/**
 * Show current modem configuration (AT&V)
 */
int modem_show_configuration(modem_t *modem)
{
    char line[SMALL_BUFFER_SIZE];

    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Configuration header */
    modem_send_response_fmt(modem, "ACTIVE PROFILE:");

    /* Basic settings */
    snprintf(line, sizeof(line), "E%d Q%d V%d X%d",
             modem->settings.echo ? 1 : 0,
             modem->settings.quiet ? 1 : 0,
             modem->settings.verbose ? 1 : 0,
             modem->settings.result_mode);
    modem_send_response_fmt(modem, "%s", line);

    /* & settings */
    snprintf(line, sizeof(line), "&C%d &D%d &S%d",
             modem->settings.dcd_mode,
             modem->settings.dtr_mode,
             modem->settings.dsr_mode);
    modem_send_response_fmt(modem, "%s", line);

    /* Other settings */
    snprintf(line, sizeof(line), "B%d L%d M%d \\N%d",
             modem->settings.bell_mode,
             modem->settings.speaker_volume,
             modem->settings.speaker_control,
             modem->settings.error_correction);
    modem_send_response_fmt(modem, "%s", line);

    /* S-registers (first 16) */
    modem_send_response_fmt(modem, "");
    modem_send_response_fmt(modem, "S-REGISTERS:");
    for (int i = 0; i < 16; i++) {
        if (i % 4 == 0) {
            if (i > 0) {
                modem_send_response_fmt(modem, "%s", line);
            }
            line[0] = '\0';
        }
        char reg_str[20];
        snprintf(reg_str, sizeof(reg_str), "S%02d:%03d ",
                i, modem->settings.s_registers[i]);
        strncat(line, reg_str, sizeof(line) - strlen(line) - 1);
    }
    if (line[0] != '\0') {
        modem_send_response_fmt(modem, "%s", line);
    }

    return modem_send_response(modem, MODEM_RESP_OK);
}

/**
 * Process unsolicited message from hardware modem (RING, CONNECT, etc.)
 * Returns true if a hardware message was detected and processed
 */
bool modem_process_hardware_message(modem_t *modem, const char *data, size_t len)
{
    bool hardware_msg_detected = false;
    time_t now = time(NULL);

    if (modem == NULL || data == NULL || len == 0) {
        return false;
    }

    /* Debug output with hex dump for better visibility */
    printf("[DEBUG] Hardware modem data (%zu bytes): ", len);
    for (size_t i = 0; i < len && i < 32; i++) {
        printf("%02X ", (unsigned char)data[i]);
    }
    printf("\n");
    printf("[DEBUG] Hardware modem ASCII: [%.*s]\n", (int)len, data);
    fflush(stdout);

    MB_LOG_DEBUG("Hardware modem data (%zu bytes): [%.*s]", len, (int)len, data);

    /* === ONLINE STATE: Maintain small buffer for disconnection detection === */
    if (modem->state == MODEM_STATE_ONLINE) {
        /* Append data to buffer for disconnection message detection */
        /* Keep buffer small (max 64 bytes) to avoid accumulating user data */
        size_t space_left = sizeof(modem->hw_msg_buffer) - modem->hw_msg_len - 1;
        size_t copy_len = MIN(len, space_left);

        if (copy_len > 0) {
            memcpy(modem->hw_msg_buffer + modem->hw_msg_len, data, copy_len);
            modem->hw_msg_len += copy_len;
            modem->hw_msg_buffer[modem->hw_msg_len] = '\0';
            modem->hw_msg_last_time = now;
        }

        /* Check if buffer contains line ending (message complete) or exceeds limit */
        bool has_line_ending = (strchr(modem->hw_msg_buffer, '\r') != NULL ||
                                strchr(modem->hw_msg_buffer, '\n') != NULL);
        bool exceeds_limit = (modem->hw_msg_len > 64);

        if (has_line_ending || exceeds_limit) {
            /* Buffer contains complete line or is too long - check for disconnection */
            if (strstr(modem->hw_msg_buffer, "NO CARRIER") != NULL ||
                strstr(modem->hw_msg_buffer, "NO DIALTONE") != NULL ||
                strstr(modem->hw_msg_buffer, "BUSY") != NULL) {

                printf("[INFO] *** DISCONNECTION detected from hardware modem ***\n");
                printf("[INFO] Message in buffer: [%s]\n", modem->hw_msg_buffer);
                fflush(stdout);
                MB_LOG_INFO("*** DISCONNECTION detected from hardware modem ***");
                MB_LOG_INFO("Message in buffer: [%s]", modem->hw_msg_buffer);

                /* Trigger DCD falling edge */
                modem_process_dcd_change(modem, false);

                modem->state = MODEM_STATE_DISCONNECTED;
                modem->online = false;
                modem->carrier = false;

                hardware_msg_detected = true;
            } else {
                MB_LOG_DEBUG("ONLINE: Line complete or buffer full - no disconnection message, clearing");
            }

            /* Clear buffer after checking (whether disconnection found or not) */
            modem->hw_msg_len = 0;
            memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
        }

        /* ONLINE state: Return to allow user data to be processed by Level 3 pipeline */
        /* Note: Small buffer maintained for disconnection detection only */
        return hardware_msg_detected;
    }

    /* === NON-ONLINE STATES: Process hardware messages normally === */

    /* Check for timeout - clear buffer if data is too old (20 seconds) */
    if (modem->hw_msg_len > 0 && (now - modem->hw_msg_last_time) > 20) {
        MB_LOG_DEBUG("Hardware message buffer timeout - clearing old data");
        modem->hw_msg_len = 0;
        memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
    }

    /* Append new data to hardware message buffer */
    size_t space_left = sizeof(modem->hw_msg_buffer) - modem->hw_msg_len - 1;
    size_t copy_len = MIN(len, space_left);
    if (copy_len > 0) {
        memcpy(modem->hw_msg_buffer + modem->hw_msg_len, data, copy_len);
        modem->hw_msg_len += copy_len;
        modem->hw_msg_buffer[modem->hw_msg_len] = '\0';
        modem->hw_msg_last_time = now;
    }

    printf("[DEBUG] Hardware message buffer (%zu bytes): [%s]\n",
           modem->hw_msg_len, modem->hw_msg_buffer);
    fflush(stdout);

    /* Process complete messages from the buffer */
    char *buffer = modem->hw_msg_buffer;

    /* Check for RING message from hardware modem */
    if (strstr(buffer, "RING") != NULL) {
        MB_LOG_INFO("*** RING detected from hardware modem ***");

        /* Increment ring counter */
        modem->settings.s_registers[SREG_RING_COUNT]++;
        int ring_count = modem->settings.s_registers[SREG_RING_COUNT];
        int auto_answer = modem->settings.s_registers[SREG_AUTO_ANSWER];

        printf("[INFO] Ring count: %d, Auto-answer setting (S0): %d\n",
               ring_count, auto_answer);
        fflush(stdout);
        MB_LOG_INFO("Ring count: %d, Auto-answer setting (S0): %d",
                   ring_count, auto_answer);

        hardware_msg_detected = true;

        /* Check if we should answer the call */
        if (auto_answer > 0 && ring_count >= auto_answer) {
            /* Hardware auto-answer mode (S0 > 0) */
            MB_LOG_INFO("*** Hardware auto-answer threshold reached (%d rings) ***", ring_count);
            MB_LOG_INFO("Hardware modem (S0=%d) will answer this call automatically", auto_answer);

            /* Hardware modem will send ATA automatically due to S0 setting */
            /* Software just needs to transition to CONNECTING state */
            modem->state = MODEM_STATE_CONNECTING;

            /* Reset ring counter */
            modem->settings.s_registers[SREG_RING_COUNT] = 0;

            MB_LOG_INFO("Modem state changed to CONNECTING - waiting for hardware CONNECT");
        } else if (auto_answer == 0 && ring_count >= 2) {
            /* Software auto-answer mode (S0=0): Answer after 2 rings */
            printf("[INFO] === SOFTWARE AUTO-ANSWER MODE (S0=0) ===\n");
            printf("[INFO] Ring threshold reached: %d/2 rings - sending ATA to hardware modem\n", ring_count);
            fflush(stdout);
            MB_LOG_INFO("=== SOFTWARE AUTO-ANSWER MODE (S0=0) ===");
            MB_LOG_INFO("Ring threshold reached: %d/2 rings - sending ATA to hardware modem", ring_count);

            /* Send ATA command to hardware modem */
            const char ata_cmd[] = "ATA\r\n";
            ssize_t sent = serial_write(modem->serial, (const unsigned char *)ata_cmd, strlen(ata_cmd));
            if (sent > 0) {
                printf("[INFO] ATA command sent to hardware modem (%zd bytes) - waiting for CONNECT response\n", sent);
                fflush(stdout);
                MB_LOG_INFO("ATA command sent to hardware modem (%zd bytes) - waiting for CONNECT response", sent);

                /* Transition to CONNECTING state */
                modem->state = MODEM_STATE_CONNECTING;

                /* Reset ring counter */
                modem->settings.s_registers[SREG_RING_COUNT] = 0;

                printf("[INFO] Modem state: CONNECTING (software-initiated)\n");
                fflush(stdout);
                MB_LOG_INFO("Modem state: CONNECTING (software-initiated)");
            } else {
                printf("[ERROR] Failed to send ATA command: %s\n", strerror(errno));
                fflush(stdout);
                MB_LOG_ERROR("Failed to send ATA command: %s", strerror(errno));
            }
        }

        /* Clear the buffer after processing RING */
        modem->hw_msg_len = 0;
        memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
    }

    /* Check for CONNECT message from hardware modem */
    /* Note: Some modems send "CONNECT 2400", "CONNECT 1200/ARQ", etc. */
    else if (strstr(buffer, "CONNECT") != NULL) {
        /* Make sure we have a complete CONNECT message (ends with \r\n) */
        char *connect_pos = strstr(buffer, "CONNECT");
        if (connect_pos != NULL) {
            /* Wait for line ending to ensure complete message */
            char *line_end = strstr(connect_pos, "\r");
            if (!line_end) line_end = strstr(connect_pos, "\n");

            if (line_end) {
                /* Complete CONNECT message received */
                printf("[INFO] *** CONNECT detected from hardware modem ***\n");
                printf("[INFO] Full message: [%s]\n", buffer);
                fflush(stdout);
                MB_LOG_INFO("*** CONNECT detected from hardware modem ***");
                MB_LOG_INFO("Full message: [%s]", buffer);

                /* Extract baudrate if present (e.g., "CONNECT 1200" or "CONNECT 1200/ARQ") */
                int baudrate = 0;
                char *space = strchr(connect_pos, ' ');
                if (space) {
                    baudrate = atoi(space + 1);
                    printf("[INFO] Connection speed: %d baud\n", baudrate);
                    fflush(stdout);
                    MB_LOG_INFO("Connection speed: %d baud", baudrate);

                    /* Dynamic speed adjustment based on CONNECT response (modem_sample pattern) */
                    if (baudrate > 0 && modem->serial) {
                        printf("[INFO] Adjusting serial port speed to match connection: %d baud\n", baudrate);
                        fflush(stdout);
                        MB_LOG_INFO("Adjusting serial port speed to match connection: %d baud", baudrate);

                        /* Convert baudrate to speed_t */
                        speed_t new_speed = modem_baudrate_to_speed_t(baudrate);

                        /* Adjust serial port speed */
                        int rc = serial_set_baudrate(modem->serial, new_speed);
                        if (rc == SUCCESS) {
                            printf("[INFO] Serial port speed adjusted successfully to %d baud\n", baudrate);
                            fflush(stdout);
                            MB_LOG_INFO("Serial port speed adjusted successfully to %d baud", baudrate);
                        } else {
                            printf("[WARNING] Failed to adjust serial port speed, continuing with current speed\n");
                            fflush(stdout);
                            MB_LOG_WARNING("Failed to adjust serial port speed, continuing with current speed");
                        }

                        /* Small delay for hardware stabilization */
                        usleep(50000);  /* 50ms */
                    }
                }

                /* Hardware modem connected - we should go online */
                modem->state = MODEM_STATE_ONLINE;
                modem->online = true;
                modem->carrier = true;
                hardware_msg_detected = true;

                printf("[INFO] Modem state changed: ONLINE=true, carrier=true\n");
                printf("[INFO] Connection established at %d baud\n", baudrate);
                fflush(stdout);

                /* Trigger DCD rising edge event for Level 3 integration */
                printf("[INFO] Triggering DCD rising edge event (carrier detected)\n");
                fflush(stdout);
                MB_LOG_INFO("Triggering DCD rising edge event (carrier detected)");
                modem_process_dcd_change(modem, true);

                /* Clear the buffer after processing CONNECT */
                modem->hw_msg_len = 0;
                memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
            } else {
                /* Partial CONNECT message - wait for completion */
                printf("[DEBUG] Partial CONNECT message detected (len=%zu) - waiting for line ending\n",
                       strlen(connect_pos));
                printf("[DEBUG] Partial buffer: [%s]\n", buffer);
                fflush(stdout);
                MB_LOG_DEBUG("Partial CONNECT message - waiting for completion (current len=%zu)",
                            strlen(connect_pos));
            }
        }
    }

    /* Check for NO CARRIER from hardware modem */
    else if (strstr(buffer, "NO CARRIER") != NULL || strstr(buffer, "NO CAR") != NULL) {
        MB_LOG_INFO("*** NO CARRIER detected from hardware modem ***");

        /* Use enhanced immediate termination function */
        modem_handle_no_carrier_termination(modem);
        hardware_msg_detected = true;

        /* Clear the buffer after processing NO CARRIER */
        modem->hw_msg_len = 0;
        memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
    }

    /* If we have a line ending at the end of buffer, check if it's a complete message */
    else if (modem->hw_msg_len > 0) {
        /* Only clear if we have a complete unrecognized line ending with \r\n */
        size_t len = strlen(buffer);
        if (len >= 2 && buffer[len-2] == '\r' && buffer[len-1] == '\n') {
            /* Check if this might be start of CONNECT or other message */
            /* Don't clear if buffer ends with potential start of message */
            if (len <= 3 || /* Very short, might be start of message like "\r\nC" */
                strstr(buffer, "CON") != NULL || /* Partial CONNECT */
                strstr(buffer, "RIN") != NULL || /* Partial RING */
                strstr(buffer, "NO ") != NULL) { /* Partial NO CARRIER */
                /* Keep buffer, might be incomplete message */
                MB_LOG_DEBUG("Keeping partial message in buffer: [%s]", buffer);
            } else {
                /* Complete line but unrecognized - clear buffer */
                MB_LOG_DEBUG("Clearing unrecognized complete message: [%s]", buffer);
                modem->hw_msg_len = 0;
                memset(modem->hw_msg_buffer, 0, sizeof(modem->hw_msg_buffer));
            }
        }
    }

    return hardware_msg_detected;
}

/**
 * Process incoming data from serial port
 */
ssize_t modem_process_input(modem_t *modem, const char *data, size_t len)
{
    if (modem == NULL || data == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* In online mode, check for escape sequence with enhanced S2/S12 support */
    if (modem->online) {
        size_t escape_consumed = 0;
        bool escape_detected = modem_check_escape_sequence(modem, data, len, &escape_consumed);

        if (escape_detected) {
            MB_LOG_INFO("Escape sequence detected, entering command mode");
            modem_go_offline(modem);
            modem_send_response(modem, MODEM_RESP_OK);
            return escape_consumed;
        }

        /* In online mode, data passes through (minus any escape sequence bytes) */
        return len;
    }

    /* In command mode, process AT commands with enhanced Hayes filtering */
    size_t filtered_bytes = modem_filter_hayes_data(modem, data, len, true);
    size_t remaining_bytes = len - filtered_bytes;
    const char *process_data = data + filtered_bytes;

    for (size_t i = 0; i < remaining_bytes; i++) {
        char c = process_data[i];
        int cr_char = modem->settings.s_registers[SREG_CR_CHAR];
        int bs_char = modem->settings.s_registers[SREG_BS_CHAR];

        /* Enhanced echo handling */
        if (c != '\0') {
            modem_handle_command_echo(modem, &c, 1);
        }

        /* Handle backspace */
        if (c == bs_char || c == 127) {
            if (modem->cmd_len > 0) {
                modem->cmd_len--;
                /* Echo backspace sequence if echo enabled */
                if (modem->settings.echo) {
                    const char bs_seq[] = "\b \b";
                    serial_write(modem->serial, bs_seq, 3);
                }
            }
            filtered_bytes++;
            continue;
        }

        /* Handle carriage return - execute command */
        if (c == cr_char || c == '\n') {
            filtered_bytes++;

            if (modem->cmd_len == 0) {
                /* Empty line - send OK */
                modem_send_response(modem, MODEM_RESP_OK);
                continue;
            }

            /* Null-terminate command */
            modem->cmd_buffer[modem->cmd_len] = '\0';

            /* Check for AT prefix */
            if (modem->cmd_len >= 2 &&
                toupper((unsigned char)modem->cmd_buffer[0]) == 'A' &&
                toupper((unsigned char)modem->cmd_buffer[1]) == 'T') {
                /* Process AT command (skip "AT" prefix) */
                modem_process_command(modem, modem->cmd_buffer + 2);
            } else if (modem->cmd_len == 1 && toupper((unsigned char)modem->cmd_buffer[0]) == 'A') {
                /* Just "A" - repeat last command or answer */
                modem_answer(modem);
            } else {
                /* Invalid command */
                MB_LOG_WARNING("Invalid command: %s", modem->cmd_buffer);
                modem_send_response(modem, MODEM_RESP_ERROR);
            }

            /* Clear command buffer */
            modem->cmd_len = 0;
            continue;
        }

        /* Add character to command buffer */
        if (modem->cmd_len < sizeof(modem->cmd_buffer) - 1) {
            modem->cmd_buffer[modem->cmd_len++] = c;
        } else {
            MB_LOG_WARNING("Command buffer overflow");
            modem->cmd_len = 0;
            modem_send_response(modem, MODEM_RESP_ERROR);
        }

        filtered_bytes++;
    }

    return filtered_bytes;
}

/* ========================================================================
 * Extended functions from modem_sample
 * ======================================================================== */

/**
 * Convert baudrate integer to speed_t
 * Based on modem_sample/serial_port.c:get_baudrate()
 */
speed_t modem_baudrate_to_speed_t(int baudrate)
{
    switch (baudrate) {
        case 300:    return B300;
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:
            MB_LOG_WARNING("Unsupported baudrate %d, using 9600", baudrate);
            return B9600;
    }
}

/**
 * Parse connection speed from CONNECT response
 * Based on modem_sample/modem_control.c:parse_connect_speed()
 */
int modem_parse_connect_speed(const char *connect_str)
{
    int speed = 0;
    const char *ptr;
    char temp_str[64];

    if (!connect_str) {
        MB_LOG_ERROR("parse_connect_speed: connect_str is NULL");
        return -1;
    }

    /* Log the raw CONNECT response for debugging */
    MB_LOG_DEBUG("Parsing CONNECT response: '%s'", connect_str);

    /* Find "CONNECT" */
    ptr = strstr(connect_str, "CONNECT");
    if (!ptr) {
        MB_LOG_ERROR("No 'CONNECT' found in response string");
        return -1;
    }

    /* Skip "CONNECT" and any spaces */
    ptr += 7; /* length of "CONNECT" */
    while (*ptr == ' ' || *ptr == '\t') {
        ptr++;
    }

    /* Copy the speed part for analysis */
    strncpy(temp_str, ptr, sizeof(temp_str) - 1);
    temp_str[sizeof(temp_str) - 1] = '\0';

    /* Remove any trailing protocol information */
    char *slash_pos = strchr(temp_str, '/');
    if (slash_pos) {
        *slash_pos = '\0';
        MB_LOG_DEBUG("Protocol detected: '%s'", slash_pos + 1);
    }

    /* Remove any trailing spaces or newlines */
    char *end = temp_str + strlen(temp_str) - 1;
    while (end > temp_str && (*end == ' ' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }

    /* Extract the speed number */
    if (sscanf(temp_str, "%d", &speed) == 1) {
        MB_LOG_DEBUG("Successfully parsed connection speed: %d bps", speed);

        /* Validate speed is reasonable */
        if (speed < 300 || speed > 115200) {
            MB_LOG_WARNING("Unusual speed %d bps - may be incorrect", speed);
        }

        return speed;
    }

    /* Check for common CONNECT variations */
    if (strlen(temp_str) == 0) {
        MB_LOG_DEBUG("CONNECT without speed - assuming 300 bps (legacy modem)");
        return 300;
    }

    /* Try to extract from more complex formats */
    if (strstr(temp_str, "1200")) speed = 1200;
    else if (strstr(temp_str, "2400")) speed = 2400;
    else if (strstr(temp_str, "4800")) speed = 4800;
    else if (strstr(temp_str, "9600")) speed = 9600;
    else if (strstr(temp_str, "19200")) speed = 19200;
    else if (strstr(temp_str, "38400")) speed = 38400;
    else if (strstr(temp_str, "57600")) speed = 57600;
    else if (strstr(temp_str, "115200")) speed = 115200;

    if (speed > 0) {
        MB_LOG_DEBUG("Extracted speed from complex format: %d bps", speed);
        return speed;
    }

    MB_LOG_ERROR("Failed to parse speed from CONNECT response: '%s'", temp_str);
    return -1;
}

/**
 * Send AT command and wait for response (synchronous)
 * Based on modem_sample/modem_control.c:send_at_command()
 */
int modem_send_at_command(modem_t *modem, const char *command,
                          char *response, size_t resp_size, int timeout_sec)
{
    char cmd_buf[LINE_BUFFER_SIZE];
    char line_buf[LINE_BUFFER_SIZE];
    int len;
    ssize_t rc;
    time_t start_time, current_time;
    int remaining_timeout;

    if (modem == NULL || command == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (modem->serial == NULL || !serial_is_open(modem->serial)) {
        MB_LOG_ERROR("Serial port not open");
        return ERROR_IO;
    }

    /* Clear response buffer if provided */
    if (response && resp_size > 0) {
        response[0] = '\0';
    }

    /* Flush input buffer before sending command */
    serial_flush(modem->serial, TCIFLUSH);

    /* Prepare command with CR terminator */
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r", command);
    len = strlen(cmd_buf);

    MB_LOG_INFO("Sending AT command: %s", command);

    /* Send command */
    rc = serial_write(modem->serial, cmd_buf, len);
    if (rc < 0) {
        MB_LOG_ERROR("Failed to send AT command");
        return ERROR_IO;
    }

    /* Small delay after sending */
    usleep(100000); /* 100ms */

    /* Read response lines until OK or ERROR or timeout */
    start_time = time(NULL);

    while (1) {
        current_time = time(NULL);
        remaining_timeout = timeout_sec - (current_time - start_time);

        if (remaining_timeout <= 0) {
            MB_LOG_ERROR("Timeout waiting for modem response");
            return ERROR_TIMEOUT;
        }

        rc = serial_read_line(modem->serial, line_buf, sizeof(line_buf), remaining_timeout);

        if (rc == ERROR_TIMEOUT) {
            MB_LOG_ERROR("Timeout reading modem response");
            return ERROR_TIMEOUT;
        } else if (rc < 0) {
            MB_LOG_ERROR("Error reading modem response: %zd", rc);
            return ERROR_IO;
        }

        if (rc > 0) {
            /* Print received line */
            MB_LOG_DEBUG("Received: %s", line_buf);

            /* Store response if buffer provided */
            if (response && resp_size > 0) {
                size_t current_len = strlen(response);
                if (current_len + rc + 2 < resp_size) {
                    if (response[0] != '\0') {
                        strcat(response, "\n");
                    }
                    strcat(response, line_buf);
                }
            }

            /* Check for CONNECT (successful connection) */
            if (strstr(line_buf, "CONNECT") != NULL) {
                MB_LOG_INFO("Modem connected: %s", line_buf);
                return SUCCESS;
            }

            /* Check for connection errors */
            if (strstr(line_buf, "NO CARRIER") != NULL) {
                MB_LOG_ERROR("Connection failed: NO CARRIER");
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "BUSY") != NULL) {
                MB_LOG_ERROR("Connection failed: BUSY");
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "NO DIALTONE") != NULL) {
                MB_LOG_ERROR("Connection failed: NO DIALTONE");
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "NO ANSWER") != NULL) {
                MB_LOG_ERROR("Connection failed: NO ANSWER");
                return ERROR_MODEM;
            }

            /* Check for OK */
            if (strstr(line_buf, "OK") != NULL) {
                MB_LOG_DEBUG("Modem responded: OK");
                return SUCCESS;
            }

            /* Check for ERROR */
            if (strstr(line_buf, "ERROR") != NULL) {
                MB_LOG_ERROR("Modem returned ERROR");
                return ERROR_MODEM;
            }
        }
    }

    return SUCCESS;
}

/**
 * Send compound AT command string (semicolon-separated)
 * Based on modem_sample/modem_control.c:send_command_string()
 */
int modem_send_command_string(modem_t *modem, const char *cmd_string, int timeout_sec)
{
    char *commands, *cmd, *saveptr;
    char response[BUFFER_SIZE];
    int rc;

    if (modem == NULL || cmd_string == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (strlen(cmd_string) == 0) {
        return SUCCESS;
    }

    MB_LOG_INFO("Sending compound command: %s", cmd_string);

    /* Make a copy of the command string for parsing */
    commands = strdup(cmd_string);
    if (!commands) {
        return ERROR_GENERAL;
    }

    /* Parse commands separated by semicolon */
    cmd = strtok_r(commands, ";", &saveptr);

    while (cmd != NULL) {
        /* Trim leading spaces */
        while (*cmd == ' ' || *cmd == '\t') {
            cmd++;
        }

        if (strlen(cmd) > 0) {
            rc = modem_send_at_command(modem, cmd, response, sizeof(response), timeout_sec);
            if (rc != SUCCESS) {
                MB_LOG_ERROR("Command failed: %s", cmd);
                free(commands);
                return rc;
            }

            /* Small delay between commands */
            usleep(200000); /* 200ms */
        }

        cmd = strtok_r(NULL, ";", &saveptr);
    }

    free(commands);
    MB_LOG_INFO("Compound command completed successfully");
    return SUCCESS;
}

/**
 * Wait for RING signal and optional connection
 * Based on modem_sample/modem_sample.c:wait_for_ring()
 */
int modem_wait_for_ring(modem_t *modem, int timeout_sec, int *connected_speed)
{
    char line_buf[LINE_BUFFER_SIZE];
    int ring_count = 0;
    time_t start_time, current_time;
    int remaining_timeout;
    ssize_t rc;
    int auto_answer;

    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (modem->serial == NULL || !serial_is_open(modem->serial)) {
        MB_LOG_ERROR("Serial port not open");
        return ERROR_IO;
    }

    /* Initialize connected_speed */
    if (connected_speed) {
        *connected_speed = -1;
    }

    auto_answer = modem->settings.s_registers[SREG_AUTO_ANSWER];

    MB_LOG_INFO("Starting serial port monitoring for RING...");

    if (auto_answer > 0) {
        /* HARDWARE mode: Wait for RING, modem will auto-answer */
        MB_LOG_INFO("HARDWARE mode: Waiting for RING signal (modem will auto-answer after %d rings)...",
                   auto_answer);
    } else {
        /* SOFTWARE mode: Wait for 2 RINGs to manually answer */
        MB_LOG_INFO("SOFTWARE mode: Waiting for RING signal (need 2 times for manual answer)...");
    }

    start_time = time(NULL);

    while (1) {
        current_time = time(NULL);
        remaining_timeout = timeout_sec - (current_time - start_time);

        if (remaining_timeout <= 0) {
            MB_LOG_ERROR("Timeout waiting for RING/CONNECT signal");
            return ERROR_TIMEOUT;
        }

        /* Read line from serial port */
        rc = serial_read_line(modem->serial, line_buf, sizeof(line_buf), 5); /* 5 second timeout per read */

        if (rc == ERROR_TIMEOUT) {
            /* Continue waiting */
            continue;
        } else if (rc < 0) {
            MB_LOG_ERROR("Error reading from serial port");
            return ERROR_IO;
        }

        if (rc > 0) {
            MB_LOG_INFO("Received: %s", line_buf);

            if (auto_answer > 0) {
                /* HARDWARE mode: Check for CONNECT (modem auto-answered) */
                if (strstr(line_buf, "CONNECT") != NULL) {
                    MB_LOG_INFO("Modem auto-answered and connected: %s", line_buf);

                    /* Parse connection speed from CONNECT response */
                    if (connected_speed) {
                        int speed = modem_parse_connect_speed(line_buf);
                        if (speed > 0) {
                            *connected_speed = speed;
                        }
                    }

                    return SUCCESS;
                }

                /* Also count RINGs for logging */
                if (strstr(line_buf, "RING") != NULL) {
                    ring_count++;
                    MB_LOG_INFO("RING detected! (count: %d) - waiting for modem auto-answer...", ring_count);
                }
            } else {
                /* SOFTWARE mode: Count RINGs for manual answer */
                if (strstr(line_buf, "RING") != NULL) {
                    ring_count++;
                    MB_LOG_INFO("RING detected! (count: %d/2)", ring_count);

                    if (ring_count >= 2) {
                        MB_LOG_INFO("RING signal detected 2 times - Ready to answer call manually");
                        return SUCCESS;
                    }
                }
            }
        }
    }

    return ERROR_TIMEOUT;
}

/**
 * Answer incoming call (send ATA and wait for CONNECT)
 * Based on modem_sample/modem_control.c:modem_answer_with_speed_adjust()
 */
int modem_answer_call(modem_t *modem, int *connected_speed)
{
    char line_buf[LINE_BUFFER_SIZE];
    ssize_t rc;
    int speed = -1;
    time_t start_time, current_time;
    int remaining_timeout;
    const int ata_timeout = 60; /* 60 seconds for ATA */

    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (modem->serial == NULL || !serial_is_open(modem->serial)) {
        MB_LOG_ERROR("Serial port not open");
        return ERROR_IO;
    }

    MB_LOG_INFO("Answering incoming call (ATA) with speed detection...");

    /* Flush input buffer before sending command */
    serial_flush(modem->serial, TCIFLUSH);

    /* Send ATA command */
    rc = serial_write(modem->serial, "ATA\r", 4);
    if (rc < 0) {
        MB_LOG_ERROR("Failed to send ATA command");
        return ERROR_IO;
    }

    /* Wait for CONNECT response */
    start_time = time(NULL);

    while (1) {
        current_time = time(NULL);
        remaining_timeout = ata_timeout - (current_time - start_time);

        if (remaining_timeout <= 0) {
            MB_LOG_ERROR("Timeout waiting for modem response");
            return ERROR_TIMEOUT;
        }

        rc = serial_read_line(modem->serial, line_buf, sizeof(line_buf), remaining_timeout);

        if (rc == ERROR_TIMEOUT) {
            MB_LOG_ERROR("Timeout reading modem response");
            return ERROR_TIMEOUT;
        } else if (rc < 0) {
            MB_LOG_ERROR("Error reading modem response");
            return ERROR_IO;
        }

        if (rc > 0) {
            MB_LOG_INFO("Received: %s", line_buf);

            /* Check for CONNECT */
            if (strstr(line_buf, "CONNECT") != NULL) {
                MB_LOG_INFO("Modem connected: %s", line_buf);

                /* Parse speed from CONNECT response */
                speed = modem_parse_connect_speed(line_buf);
                if (speed > 0 && connected_speed) {
                    *connected_speed = speed;
                }

                return SUCCESS;
            }

            /* Check for errors */
            if (strstr(line_buf, "NO CARRIER") != NULL) {
                MB_LOG_ERROR("Connection failed: NO CARRIER");
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "BUSY") != NULL) {
                MB_LOG_ERROR("Connection failed: BUSY");
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "NO ANSWER") != NULL) {
                MB_LOG_ERROR("Connection failed: NO ANSWER");
                return ERROR_MODEM;
            }
        }
    }

    return SUCCESS;
}

/* ========================================================================
 * DCD Monitoring Functions - Level 1 Enhancement
 * ======================================================================== */

/**
 * Monitor DCD signal and update modem state accordingly
 * Implements Level 1 DCD-based state transition strengthening
 */
int modem_monitor_dcd_signal(modem_t *modem)
{
    bool current_dcd_state;
    time_t now = time(NULL);

    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!modem->dcd_monitoring_enabled) {
        /* DCD monitoring is disabled */
        return SUCCESS;
    }

    if (modem->serial == NULL || !serial_is_open(modem->serial)) {
        MB_LOG_WARNING("Cannot monitor DCD: serial port not available");
        return ERROR_IO;
    }

    /* Rate limit DCD checks to avoid excessive system calls */
    if ((now - modem->last_dcd_check_time) < 1) {
        return SUCCESS;  /* Skip if checked within last second */
    }

    /* Get current DCD state from hardware */
    int dcd_status = serial_get_dcd(modem->serial);
    if (dcd_status < 0) {
        MB_LOG_WARNING("Failed to read DCD signal");
        return ERROR_IO;
    }

    current_dcd_state = (dcd_status > 0);
    modem->last_dcd_check_time = now;

    /* Process DCD state change if detected */
    if (current_dcd_state != modem->last_dcd_state) {
        MB_LOG_INFO("DCD signal changed: %s -> %s",
                   modem->last_dcd_state ? "HIGH" : "LOW",
                   current_dcd_state ? "HIGH" : "LOW");

        return modem_process_dcd_change(modem, current_dcd_state);
    }

    return SUCCESS;
}

/**
 * Enable/disable DCD-based state transitions
 */
int modem_set_dcd_monitoring(modem_t *modem, bool enabled)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("DCD monitoring %s", enabled ? "ENABLED" : "DISABLED");

    modem->dcd_monitoring_enabled = enabled;

    if (enabled && modem->serial && serial_is_open(modem->serial)) {
        /* Initialize DCD state when enabling */
        int dcd_status = serial_get_dcd(modem->serial);
        if (dcd_status >= 0) {
            modem->last_dcd_state = (dcd_status > 0);
            modem->last_dcd_check_time = time(NULL);

            MB_LOG_DEBUG("Initial DCD state: %s",
                        modem->last_dcd_state ? "HIGH" : "LOW");

            /* Enable carrier detection in serial port */
            serial_enable_carrier_detect(modem->serial);
        } else {
            MB_LOG_WARNING("Failed to read initial DCD state");
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Check if DCD monitoring is enabled
 */
bool modem_is_dcd_monitoring_enabled(modem_t *modem)
{
    if (modem == NULL) {
        return false;
    }

    return modem->dcd_monitoring_enabled;
}

/**
 * Process DCD state change (DCD rising/falling edge detection)
 * Implements reliable DCD-based state transitions per LEVEL3_WORK_TODO.txt
 */
int modem_process_dcd_change(modem_t *modem, bool dcd_state)
{
    modem_state_t old_state;

    printf("[DEBUG-DCD] modem_process_dcd_change() called: modem=%p, dcd_state=%d\n", (void*)modem, dcd_state);
    fflush(stdout);

    if (modem == NULL) {
        printf("[ERROR-DCD] modem is NULL!\n");
        fflush(stdout);
        return ERROR_INVALID_ARG;
    }

    old_state = modem->state;
    printf("[DEBUG-DCD] old_state=%s, dcd_monitoring=%d, callback=%p\n",
           modem_state_to_string(old_state),
           modem->dcd_monitoring_enabled,
           (void*)modem->dcd_event_callback);
    fflush(stdout);

    /* Store the new DCD state */
    modem->last_dcd_state = dcd_state;

    printf("[INFO-DCD] Processing DCD change: %s (state: %s)\n",
           dcd_state ? "RISE" : "FALL",
           modem_state_to_string(old_state));
    fflush(stdout);
    MB_LOG_INFO("Processing DCD change: %s (state: %s)",
               dcd_state ? "RISE" : "FALL",
               modem_state_to_string(old_state));

    if (dcd_state) {
        /* DCD RISING EDGE - Carrier detected */

        /* Update carrier state based on AT&C setting */
        if (modem->settings.dcd_mode == 1) {
            /* &C1: DCD follows carrier */
            modem_set_carrier(modem, true);
        }

        /* State transitions based on current state */
        switch (old_state) {
            case MODEM_STATE_DISCONNECTED:
            case MODEM_STATE_COMMAND:
                /* Carrier appeared while in command mode - this might indicate
                 * an incoming connection or hardware modem connection */
                MB_LOG_INFO("DCD rise detected in %s mode - potential connection",
                           old_state == MODEM_STATE_COMMAND ? "COMMAND" : "DISCONNECTED");

                /* For hardware modems, this might indicate CONNECT sequence */
                if (modem->serial && serial_is_open(modem->serial)) {
                    /* Check if this is a real connection by monitoring for
                     * CONNECT messages from hardware modem */
                    modem->state = MODEM_STATE_CONNECTING;
                    MB_LOG_INFO("State transition: %s -> CONNECTING (DCD rise)",
                               modem_state_to_string(old_state));
                }
                break;

            case MODEM_STATE_CONNECTING:
                /* DCD rose during connecting - connection established */
                modem->state = MODEM_STATE_ONLINE;
                modem->online = true;
                MB_LOG_INFO("State transition: CONNECTING -> ONLINE (DCD rise)");
                break;

            case MODEM_STATE_ONLINE:
                /* Already online - DCD rise is redundant but OK */
                MB_LOG_DEBUG("DCD rise while already online - no state change");
                break;

            case MODEM_STATE_RINGING:
                /* DCD rise during ringing typically means connection established */
                modem->state = MODEM_STATE_ONLINE;
                modem->online = true;
                MB_LOG_INFO("State transition: RINGING -> ONLINE (DCD rise)");
                break;
        }

    } else {
        /* DCD FALLING EDGE - Carrier lost */

        /* Update carrier state based on AT&C setting */
        if (modem->settings.dcd_mode == 1) {
            /* &C1: DCD follows carrier */
            modem_set_carrier(modem, false);
        }

        /* Use enhanced immediate cleanup for DCD falling edge */
        MB_LOG_INFO("DCD fall detected - using enhanced immediate cleanup");
        modem_handle_dcd_falling_cleanup(modem);

        /* Additional AT&D specific handling for DTR coordination */
        if (old_state == MODEM_STATE_ONLINE) {
            switch (modem->settings.dtr_mode) {
                case 2: /* &D2: Hang up (additional cleanup) */
                case 3: /* &D3: Reset modem (additional cleanup) */
                    MB_LOG_DEBUG("Additional DTR cleanup for &D%d mode", modem->settings.dtr_mode);
                    if (modem->serial && serial_is_open(modem->serial)) {
                        serial_flush(modem->serial, TCIFLUSH);
                        serial_flush(modem->serial, TCOFLUSH);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /* Log state transition if it occurred */
    if (old_state != modem->state) {
        MB_LOG_INFO("DCD-triggered state transition: %s -> %s",
                   modem_state_to_string(old_state),
                   modem_state_to_string(modem->state));
    }

    /* Call DCD event callback if registered (bridge integration) */
    printf("[DEBUG-DCD] About to check callback: callback=%p\n", (void*)modem->dcd_event_callback);
    fflush(stdout);
    if (modem->dcd_event_callback != NULL) {
        printf("[INFO-DCD] Calling DCD event callback with state: %s\n", dcd_state ? "RISE" : "FALL");
        fflush(stdout);
        MB_LOG_DEBUG("Calling DCD event callback with state: %s", dcd_state ? "RISE" : "FALL");
        int callback_result = modem->dcd_event_callback(modem->dcd_callback_user_data, dcd_state);
        printf("[INFO-DCD] DCD callback returned: %d\n", callback_result);
        fflush(stdout);
        if (callback_result != SUCCESS) {
            MB_LOG_WARNING("DCD event callback returned error: %d", callback_result);
        }
    } else {
        printf("[WARNING-DCD] DCD event callback is NULL - not calling\n");
        fflush(stdout);
    }

    printf("[DEBUG-DCD] modem_process_dcd_change() returning SUCCESS\n");
    fflush(stdout);
    return SUCCESS;
}

/**
 * Convert modem state to string for logging
 */
const char *modem_state_to_string(modem_state_t state)
{
    switch (state) {
        case MODEM_STATE_COMMAND:    return "COMMAND";
        case MODEM_STATE_ONLINE:     return "ONLINE";
        case MODEM_STATE_RINGING:    return "RINGING";
        case MODEM_STATE_CONNECTING: return "CONNECTING";
        case MODEM_STATE_DISCONNECTED: return "DISCONNECTED";
        default:                     return "UNKNOWN";
    }
}

/* ========================================================================
 * Level 1 Hayes Command Filtering Functions
 * ======================================================================== */

/**
 * Check if data should be filtered based on current mode and Hayes settings
 * Implements boundary clarification between command and data modes
 */
size_t modem_filter_hayes_data(modem_t *modem, const char *data, size_t len, bool is_command_mode)
{
    size_t filtered = 0;

    if (modem == NULL || data == NULL || len == 0) {
        return 0;
    }

    if (is_command_mode) {
        /* In command mode, filter out Hayes result codes and other modem responses */
        /* This prevents echo loops and duplicate responses */
        const char *hayes_responses[] = {
            "OK", "ERROR", "CONNECT", "NO CARRIER", "RING", 
            "NO DIALTONE", "BUSY", "NO ANSWER", NULL
        };

        /* Check if data starts with any known Hayes response */
        for (int i = 0; hayes_responses[i] != NULL; i++) {
            size_t resp_len = strlen(hayes_responses[i]);
            if (len >= resp_len && 
                strncmp(data, hayes_responses[i], resp_len) == 0) {
                MB_LOG_DEBUG("Filtering Hayes response in command mode: %s", hayes_responses[i]);
                filtered = len;  /* Filter entire response */
                break;
            }
        }

        /* Also filter common response prefixes with case variations */
        const char *prefixes[] = { "OK", "ERROR", "CONNECT", "NO CARRIER", "RING", NULL };
        for (int i = 0; prefixes[i] != NULL && filtered == 0; i++) {
            size_t prefix_len = strlen(prefixes[i]);
            if (len >= prefix_len) {
                /* Case-insensitive comparison for prefixes */
                bool match = true;
                for (size_t j = 0; j < prefix_len; j++) {
                    if (toupper((unsigned char)data[j]) != prefixes[i][j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    MB_LOG_DEBUG("Filtering Hayes prefix in command mode: %s", prefixes[i]);
                    filtered = prefix_len;
                    break;
                }
            }
        }
    } else {
        /* In data mode, only filter escape sequences (handled separately) */
        filtered = 0;
    }

    return filtered;
}

/**
 * Handle Hayes escape sequence detection with proper S2/S12 register support
 * Prevents accidental escape in data mode with configurable guard times
 */
bool modem_check_escape_sequence(modem_t *modem, const char *data, size_t len, size_t *consumed)
{
    char escape_char = modem_get_escape_character(modem);
    int guard_time_ms = modem_get_escape_guard_time(modem);
    time_t now = time(NULL);
    size_t i;

    if (modem == NULL || data == NULL || len == 0 || consumed == NULL) {
        *consumed = 0;
        return false;
    }

    *consumed = 0;

    for (i = 0; i < len; i++) {
        if (data[i] == escape_char) {
            /* Check guard time - S12 register value * 50ms */
            int time_diff_ms = (now - modem->last_escape_time) * 1000;
            
            if (modem->escape_count == 0 || time_diff_ms < guard_time_ms) {
                /* Within guard time - count this character */
                modem->escape_count++;
                modem->last_escape_time = now;
                MB_LOG_DEBUG("Escape sequence: character %d/%d (time diff: %dms)", 
                           modem->escape_count, 3, time_diff_ms);

                if (modem->escape_count >= 3) {
                    /* Complete escape sequence detected */
                    MB_LOG_INFO("Hayes escape sequence detected (char='%c', guard=%dms)", 
                               escape_char, guard_time_ms);
                    *consumed = i + 1;
                    modem->escape_count = 0;  /* Reset for next detection */
                    return true;
                }
            } else {
                /* Guard time violated - restart counting */
                MB_LOG_DEBUG("Guard time violated (%dms >= %dms), restarting escape count", 
                           time_diff_ms, guard_time_ms);
                modem->escape_count = 1;
                modem->last_escape_time = now;
            }
            *consumed = i + 1;  /* Consume the escape character */
        } else {
            /* Non-escape character - reset escape count */
            if (modem->escape_count > 0) {
                MB_LOG_DEBUG("Non-escape character '%c' detected, resetting escape count", data[i]);
                modem->escape_count = 0;
            }
        }
    }

    return false;
}

/**
 * Get escape guard time in milliseconds from S12 register
 */
int modem_get_escape_guard_time(modem_t *modem)
{
    if (modem == NULL) {
        return 1000;  /* Default 1 second */
    }

    /* S12 value * 50ms = guard time */
    int s12_value = modem_get_sreg(modem, SREG_ESCAPE_GUARD_TIME);
    if (s12_value <= 0) {
        s12_value = 20;  /* Default to 20 * 50ms = 1 second */
    }

    return s12_value * 50;
}

/**
 * Get escape character from S2 register
 */
char modem_get_escape_character(modem_t *modem)
{
    if (modem == NULL) {
        return '+';  /* Default escape character */
    }

    return (char)modem_get_sreg(modem, SREG_ESCAPE_CODE);
}

/**
 * Process command mode echo control with enhanced ATE handling
 */
int modem_handle_command_echo(modem_t *modem, const char *data, size_t len)
{
    if (modem == NULL || data == NULL || len == 0) {
        return ERROR_INVALID_ARG;
    }

    if (modem->settings.echo) {
        /* Enhanced echo: handle backspace, CR, LF properly */
        for (size_t i = 0; i < len; i++) {
            char c = data[i];
            int bs_char = modem->settings.s_registers[SREG_BS_CHAR];
            
            if (c == bs_char || c == 127) {
                /* Echo backspace sequence */
                const char bs_seq[] = "\b \b";
                serial_write(modem->serial, bs_seq, 3);
            } else if (c == '\r') {
                /* Echo CR as CR */
                serial_write(modem->serial, &c, 1);
            } else if (c == '\n') {
                /* Echo LF as LF */
                serial_write(modem->serial, &c, 1);
            } else {
                /* Echo regular character */
                serial_write(modem->serial, &c, 1);
            }
        }
    }

    return SUCCESS;
}

/**
 * Enhanced result code filtering based on Q, V, X register settings
 */
int modem_filter_result_code(modem_t *modem, const char *response,
                           char *filtered_response, size_t resp_size)
{
    if (modem == NULL || response == NULL || filtered_response == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (resp_size == 0) {
        return ERROR_INVALID_ARG;
    }

    /* Copy original response */
    strncpy(filtered_response, response, resp_size - 1);
    filtered_response[resp_size - 1] = '\0';

    /* Q0: Quiet mode - suppress responses */
    if (modem->settings.quiet) {
        filtered_response[0] = '\0';
        return SUCCESS;
    }

    /* V0: Numeric mode - convert responses to numeric codes */
    if (!modem->settings.verbose) {
        int code = 0;
        if (strcmp(response, MODEM_RESP_OK) == 0) code = 0;
        else if (strcmp(response, MODEM_RESP_CONNECT) == 0) code = 1;
        else if (strcmp(response, MODEM_RESP_RING) == 0) code = 2;
        else if (strcmp(response, MODEM_RESP_NO_CARRIER) == 0) code = 3;
        else if (strcmp(response, MODEM_RESP_ERROR) == 0) code = 4;
        else if (strcmp(response, MODEM_RESP_NO_DIALTONE) == 0) code = 6;
        else if (strcmp(response, MODEM_RESP_BUSY) == 0) code = 7;
        else if (strcmp(response, MODEM_RESP_NO_ANSWER) == 0) code = 8;

        snprintf(filtered_response, resp_size, "%d", code);
        return SUCCESS;
    }

    /* X mode filtering - based on ATX setting */
    int x_mode = modem->settings.result_mode;
    if (x_mode < 4) {
        /* X0-X3: Filter some responses based on mode */
        bool should_filter = false;
        
        switch (x_mode) {
            case 0: /* Basic codes only */
                if (strcmp(response, MODEM_RESP_NO_DIALTONE) == 0 ||
                    strcmp(response, MODEM_RESP_BUSY) == 0 ||
                    strcmp(response, MODEM_RESP_NO_ANSWER) == 0) {
                    should_filter = true;
                    strcpy(filtered_response, MODEM_RESP_NO_CARRIER);
                }
                break;
                
            case 1: /* X0 + connection speed */
                /* No filtering needed - default behavior */
                break;
                
            case 2: /* X1 + NO DIALTONE */
                if (strcmp(response, MODEM_RESP_BUSY) == 0 ||
                    strcmp(response, MODEM_RESP_NO_ANSWER) == 0) {
                    should_filter = true;
                    strcpy(filtered_response, MODEM_RESP_NO_CARRIER);
                }
                break;
                
            case 3: /* X1 + BUSY */
                if (strcmp(response, MODEM_RESP_NO_DIALTONE) == 0 ||
                    strcmp(response, MODEM_RESP_NO_ANSWER) == 0) {
                    should_filter = true;
                    strcpy(filtered_response, MODEM_RESP_NO_CARRIER);
                }
                break;
        }

        if (should_filter) {
            MB_LOG_DEBUG("Filtered response based on X%d mode: %s -> %s", 
                        x_mode, response, filtered_response);
        }
    }

    return SUCCESS;
}

/* ========================================================================
 * Level 1 DTR/DCD Coordination Improvement Functions
 * ======================================================================== */

/**
 * Handle DTR signal changes with normalized Hayes &D settings
 * Implements DTR signal handling normalization per LEVEL3_WORK_TODO.txt
 */
int modem_handle_dtr_change(modem_t *modem, bool dtr_state)
{
    modem_state_t old_state;
    
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }
    
    old_state = modem->state;
    
    MB_LOG_INFO("DTR signal changed: %s (current state: %s, &D%d mode)",
               dtr_state ? "HIGH" : "LOW",
               modem_state_to_string(old_state),
               modem->settings.dtr_mode);
    
    if (!dtr_state) {
        /* DTR fell (OFF) - process based on &D setting */
        switch (modem->settings.dtr_mode) {
            case 0: /* &D0: Ignore DTR */
                MB_LOG_DEBUG("&D0 mode: ignoring DTR transition");
                break;
                
            case 1: /* &D1: Go to command mode */
                MB_LOG_DEBUG("&D1 mode: DTR OFF -> command mode");
                if (modem->online) {
                    modem->state = MODEM_STATE_COMMAND;
                    modem->online = false;
                    MB_LOG_INFO("State transition: ONLINE -> COMMAND (DTR OFF, &D1)");
                }
                break;
                
            case 2: /* &D2: Hang up */
                MB_LOG_DEBUG("&D2 mode: DTR OFF -> hangup");
                if (modem->online || modem->state == MODEM_STATE_CONNECTING) {
                    MB_LOG_INFO("State transition: %s -> DISCONNECTED (DTR OFF, &D2)",
                               modem_state_to_string(old_state));
                    modem->state = MODEM_STATE_DISCONNECTED;
                    modem->online = false;
                    modem_hangup(modem);
                    /* Send NO CARRIER notification to client */
                    modem_send_no_carrier(modem);
                }
                break;
                
            case 3: /* &D3: Reset modem */
                MB_LOG_DEBUG("&D3 mode: DTR OFF -> reset modem");
                MB_LOG_INFO("State transition: %s -> COMMAND (DTR OFF, &D3 - reset)",
                           modem_state_to_string(old_state));
                modem_reset(modem);
                modem_send_no_carrier(modem);
                break;
                
            default:
                MB_LOG_WARNING("Invalid &D setting: %d", modem->settings.dtr_mode);
                break;
        }
    } else {
        /* DTR rose (ON) - typically used for reconnection */
        MB_LOG_DEBUG("DTR ON - no action required (modem ready for commands)");
        
        /* Ensure we're in command mode when DTR is ON */
        if (modem->state == MODEM_STATE_DISCONNECTED) {
            modem->state = MODEM_STATE_COMMAND;
            MB_LOG_DEBUG("State transition: DISCONNECTED -> COMMAND (DTR ON)");
        }
    }
    
    return SUCCESS;
}

/**
 * Process immediate data mode termination on NO CARRIER receipt
 * Implements immediate cleanup on NO CARRIER per LEVEL3_WORK_TODO.txt
 */
int modem_handle_no_carrier_termination(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Processing immediate data mode termination (NO CARRIER)");

    /* Immediate transition to command mode if we're in online mode */
    if (modem->online || modem->state == MODEM_STATE_ONLINE) {
        MB_LOG_INFO("Immediate data mode termination: ONLINE -> COMMAND");
        
        /* Force immediate state transition */
        modem->state = MODEM_STATE_COMMAND;
        modem->online = false;
        modem->carrier = false;
        
        /* Clear any pending escape sequences */
        modem->escape_count = 0;
        modem->last_escape_time = 0;
        
        /* Clear command buffer */
        modem->cmd_len = 0;
        memset(modem->cmd_buffer, 0, sizeof(modem->cmd_buffer));
        
        /* Reset ring counter */
        modem->settings.s_registers[SREG_RING_COUNT] = 0;

        MB_LOG_INFO("Data mode termination complete - now in COMMAND mode");

        /* Trigger DCD falling edge event for Level 3 integration */
        MB_LOG_INFO("Triggering DCD falling edge event (carrier lost)");
        modem_process_dcd_change(modem, false);

    } else if (modem->state == MODEM_STATE_CONNECTING) {
        /* Connection failed during establishment */
        MB_LOG_INFO("Connection failed: CONNECTING -> DISCONNECTED");
        modem->state = MODEM_STATE_DISCONNECTED;
        modem->online = false;
        modem->carrier = false;

        /* Reset ring counter */
        modem->settings.s_registers[SREG_RING_COUNT] = 0;

        /* Trigger DCD falling edge event for Level 3 integration */
        MB_LOG_INFO("Triggering DCD falling edge event (connection failed)");
        modem_process_dcd_change(modem, false);

    } else {
        /* Already not in online mode */
        MB_LOG_DEBUG("NO CARRIER received while in %s mode - no termination needed",
                    modem_state_to_string(modem->state));
    }
    
    return SUCCESS;
}

/**
 * Process immediate cleanup transition on DCD falling edge
 * Implements DCD fall-based immediate cleanup per LEVEL3_WORK_TODO.txt
 */
int modem_handle_dcd_falling_cleanup(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }
    
    MB_LOG_INFO("Processing immediate cleanup transition (DCD falling edge)");
    
    modem_state_t old_state = modem->state;
    
    /* Force immediate cleanup based on current state */
    switch (old_state) {
        case MODEM_STATE_ONLINE:
            /* Force immediate termination of data mode */
            MB_LOG_INFO("DCD fall: forcing immediate ONLINE -> COMMAND transition");
            
            /* Update internal state immediately */
            modem->state = MODEM_STATE_COMMAND;
            modem->online = false;
            modem->carrier = false;
            
            /* Clear all communication buffers */
            modem->cmd_len = 0;
            memset(modem->cmd_buffer, 0, sizeof(modem->cmd_buffer));
            modem->escape_count = 0;
            modem->last_escape_time = 0;
            
            /* Clear ring counter */
            modem->settings.s_registers[SREG_RING_COUNT] = 0;
            
            /* Flush serial port buffers if available */
            if (modem->serial && serial_is_open(modem->serial)) {
                serial_flush(modem->serial, TCIFLUSH);
                serial_flush(modem->serial, TCOFLUSH);
            }
            
            MB_LOG_INFO("Immediate cleanup complete: data connection terminated");
            break;
            
        case MODEM_STATE_CONNECTING:
            /* Connection attempt failed */
            MB_LOG_INFO("DCD fall: forcing immediate CONNECTING -> DISCONNECTED transition");
            modem->state = MODEM_STATE_DISCONNECTED;
            modem->online = false;
            modem->carrier = false;
            modem->settings.s_registers[SREG_RING_COUNT] = 0;
            break;
            
        case MODEM_STATE_RINGING:
            /* Ringing stopped */
            MB_LOG_INFO("DCD fall: forcing immediate RINGING -> DISCONNECTED transition");
            modem->state = MODEM_STATE_DISCONNECTED;
            modem->online = false;
            modem->carrier = false;
            modem->settings.s_registers[SREG_RING_COUNT] = 0;
            break;
            
        case MODEM_STATE_COMMAND:
        case MODEM_STATE_DISCONNECTED:
            /* Already in non-communicating state */
            MB_LOG_DEBUG("DCD fall while in %s mode - cleanup not needed",
                        modem_state_to_string(old_state));
            break;
    }
    
    /* Send NO CARRIER notification if we transitioned from an active state */
    if (old_state == MODEM_STATE_ONLINE || 
        old_state == MODEM_STATE_CONNECTING || 
        old_state == MODEM_STATE_RINGING) {
        
        MB_LOG_DEBUG("Sending NO CARRIER notification due to DCD fall");
        modem_send_no_carrier(modem);
    }
    
    return SUCCESS;
}

/**
 * Enhanced modem_go_offline with DTR/DCD coordination
 * Integrates DTR signal handling and immediate cleanup transitions
 */
int modem_go_offline_enhanced(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }
    
    MB_LOG_INFO("Enhanced offline transition with DTR/DCD coordination");
    
    modem_state_t old_state = modem->state;
    
    /* Perform standard offline transition */
    int rc = modem_go_offline(modem);
    if (rc != SUCCESS) {
        MB_LOG_WARNING("Standard offline transition failed, continuing with enhanced cleanup");
    }
    
    /* Enhanced cleanup based on previous state */
    if (old_state == MODEM_STATE_ONLINE) {
        /* Clear DTR signal when going offline (if supported by hardware) */
        if (modem->serial && serial_is_open(modem->serial)) {
            MB_LOG_DEBUG("Clearing DTR signal on offline transition");
            serial_set_dtr(modem->serial, false);
            
            /* Small delay for DTR signal to settle */
            usleep(100000); /* 100ms */
        }
        
        /* Ensure carrier state is properly cleared */
        modem_set_carrier(modem, false);
        
        MB_LOG_INFO("Enhanced offline: ONLINE -> COMMAND with DTR coordination");
    }
    
    return SUCCESS;
}

/**
 * Check and process pending DTR/DCD state transitions
 * This should be called regularly to handle signal changes
 */
int modem_process_dtr_dcd_transitions(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }
    
    if (modem->serial == NULL || !serial_is_open(modem->serial)) {
        return SUCCESS; /* Nothing to process if serial not available */
    }
    
    /* Process DCD monitoring if enabled */
    if (modem->dcd_monitoring_enabled) {
        int rc = modem_monitor_dcd_signal(modem);
        if (rc != SUCCESS) {
            MB_LOG_WARNING("DCD monitoring failed: %s", strerror(rc));
        }
    }
    
    /* Note: DTR signal monitoring would require hardware-specific
     * interrupt handling or polling. For now, DTR changes are
     * handled through modem_handle_dtr_change() when called by
     * higher-level code. */

    return SUCCESS;
}

/* ========================================================================
 * Level 1 Enhanced RING Processing Functions (modem_sample integration)
 * ======================================================================== */

/**
 * Enhanced RING detection with timing analysis
 * Based on modem_sample/modem_sample.c:detect_ring() with timing enhancements
 */
bool modem_detect_ring_enhanced(modem_t *modem, const char *line)
{
    if (modem == NULL || line == NULL) {
        return false;
    }

    /* Basic RING detection */
    if (strstr(line, "RING") == NULL) {
        return false;
    }

    /* Enhanced timing analysis */
    time_t now = time(NULL);

    /* Log first RING detection */
    if (modem->settings.s_registers[SREG_RING_COUNT] == 0) {
        MB_LOG_INFO("=== FIRST RING DETECTED ===");
        MB_LOG_DEBUG("First RING timestamp: %ld", now);

        /* Store first ring time for interval calculation */
        if (modem->hw_msg_last_time == 0) {
            modem->hw_msg_last_time = now;
        }
    } else {
        /* Calculate interval from previous ring */
        time_t ring_interval = now - modem->hw_msg_last_time;
        MB_LOG_INFO("RING #%d detected (interval: %ld seconds from previous RING)",
                   modem->settings.s_registers[SREG_RING_COUNT] + 1, ring_interval);

        /* Analyze ring interval patterns */
        if (ring_interval < 3) {
            MB_LOG_DEBUG("RING interval: FAST (%ld seconds) - typical for modern telco systems", ring_interval);
        } else if (ring_interval > 8) {
            MB_LOG_DEBUG("RING interval: SLOW (%ld seconds) - possible line issues or caller hesitation", ring_interval);
        } else {
            MB_LOG_DEBUG("RING interval: NORMAL (%ld seconds)", ring_interval);
        }

        /* Update last ring time */
        modem->hw_msg_last_time = now;
    }

    return true;
}

/**
 * Enhanced wait for RING with timing analysis and error recovery
 * Based on modem_sample/modem_sample.c:wait_for_ring() with Level 1 improvements
 */
int modem_wait_for_ring_enhanced(modem_t *modem, int timeout_sec, int *connected_speed)
{
    char line_buf[LINE_BUFFER_SIZE];
    int ring_count = 0;
    time_t start_time, current_time, last_ring_time = 0;
    int remaining_timeout;
    int auto_answer;
    int rc;

    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (modem->serial == NULL || !serial_is_open(modem->serial)) {
        MB_LOG_ERROR("Serial port not open");
        return ERROR_IO;
    }

    /* Initialize connected_speed */
    if (connected_speed) {
        *connected_speed = -1;
    }

    auto_answer = modem->settings.s_registers[SREG_AUTO_ANSWER];

    MB_LOG_INFO("=== ENHANCED RING/CONNECT MONITORING STARTED ===");
    MB_LOG_INFO("Starting serial port monitoring with timing analysis...");

    if (auto_answer > 0) {
        /* HARDWARE mode: Enhanced monitoring */
        MB_LOG_INFO("HARDWARE mode: S0=%d - modem will auto-answer after %d rings",
                   auto_answer, auto_answer);
        MB_LOG_INFO("Monitoring: RING timing, modem response, and CONNECT detection");
    } else {
        /* SOFTWARE mode: Enhanced monitoring */
        MB_LOG_INFO("SOFTWARE mode: S0=0 - manual ATA after 2 rings");
        MB_LOG_INFO("Monitoring: RING timing for software answer decision");
    }

    start_time = time(NULL);

    while (1) {
        current_time = time(NULL);
        remaining_timeout = timeout_sec - (current_time - start_time);

        if (remaining_timeout <= 0) {
            MB_LOG_ERROR("=== TIMEOUT: No RING/CONNECT detected within %d seconds ===", timeout_sec);
            MB_LOG_INFO("Possible causes:");
            MB_LOG_INFO("  - No incoming calls received");
            MB_LOG_INFO("  - Modem S0 register not properly set");
            MB_LOG_INFO("  - Serial port communication issues");
            MB_LOG_INFO("  - Hardware modem not connected");
            return ERROR_TIMEOUT;
        }

        /* Additional timeout if we've received RINGs but no CONNECT */
        if (last_ring_time > 0 && (current_time - last_ring_time) > 30) {
            MB_LOG_ERROR("=== TIMEOUT: RINGs received but no CONNECT after 30 seconds ===");
            return ERROR_TIMEOUT;
        }

        /* Read line from serial port */
        rc = serial_read_line(modem->serial, line_buf, sizeof(line_buf), 5); /* 5 second timeout */

        if (rc == ERROR_TIMEOUT) {
            /* Continue waiting */
            continue;
        } else if (rc < 0) {
            MB_LOG_ERROR("Error reading from serial port");
            return ERROR_IO;
        }

        if (rc > 0) {
            MB_LOG_DEBUG("Received: %s", line_buf);

            /* Enhanced RING detection with timing analysis */
            if (modem_detect_ring_enhanced(modem, line_buf)) {
                ring_count++;
                current_time = time(NULL);

                if (last_ring_time == 0) {
                    MB_LOG_DEBUG("First RING timestamp: %ld seconds from start",
                               current_time - start_time);
                } else {
                    MB_LOG_DEBUG("RING interval: %ld seconds", current_time - last_ring_time);

                    if (ring_count == 2) {
                        MB_LOG_INFO("=== SECOND RING DETECTED ===");
                        if (auto_answer > 0) {
                            MB_LOG_INFO("Hardware modem should auto-answer NOW (S0=%d)", auto_answer);
                        } else {
                            MB_LOG_INFO("Software mode threshold reached - ready for ATA command");
                        }
                    }
                }

                last_ring_time = current_time;

                /* Process based on auto-answer mode */
                if (auto_answer > 0) {
                    /* HARDWARE mode: Check if we've reached auto-answer threshold */
                    if (ring_count >= auto_answer) {
                        MB_LOG_INFO("=== HARDWARE AUTO-ANSWER THRESHOLD REACHED ===");
                        MB_LOG_INFO("Modem should auto-answer after %d rings (S0=%d)", ring_count, auto_answer);
                        MB_LOG_INFO("Waiting for hardware CONNECT response...");

                        /* Reset ring counter and wait for CONNECT */
                        modem->settings.s_registers[SREG_RING_COUNT] = 0;

                        /* Continue loop to wait for CONNECT */
                        continue;
                    }
                } else {
                    /* SOFTWARE mode: Answer after 2 rings */
                    if (ring_count >= 2) {
                        MB_LOG_INFO("=== SOFTWARE AUTO-ANSWER THRESHOLD REACHED ===");
                        MB_LOG_INFO("Ready to send ATA command (2 rings detected)");

                        /* Reset ring counter */
                        modem->settings.s_registers[SREG_RING_COUNT] = 0;

                        /* Return success to let caller send ATA */
                        return SUCCESS;
                    }
                }
            }

            /* Check for CONNECT response (both modes) */
            if (strstr(line_buf, "CONNECT") != NULL) {
                MB_LOG_INFO("=== CONNECT DETECTED FROM HARDWARE MODEM ===");
                MB_LOG_INFO("Full response: %s", line_buf);

                int elapsed_time = current_time - start_time;
                MB_LOG_INFO("Total connection time: %d seconds", elapsed_time);
                MB_LOG_INFO("Total rings received: %d", ring_count);

                /* Parse connection speed from CONNECT response */
                if (connected_speed) {
                    int speed = modem_parse_connect_speed(line_buf);
                    if (speed > 0) {
                        *connected_speed = speed;
                        MB_LOG_INFO("Connection speed detected: %d bps", speed);

                        /* Dynamic speed adjustment - Level 1 enhancement */
                        if (speed > 0 && modem->serial) {
                            MB_LOG_INFO("Adjusting serial port speed to match connection: %d baud", speed);

                            speed_t new_speed = modem_baudrate_to_speed_t(speed);
                            rc = serial_set_baudrate(modem->serial, new_speed);

                            if (rc == SUCCESS) {
                                MB_LOG_INFO("Serial port speed adjusted successfully to %d baud", speed);
                            } else {
                                MB_LOG_WARNING("Failed to adjust serial port speed, continuing with current speed");
                            }

                            /* Small delay for hardware stabilization */
                            usleep(50000);  /* 50ms */
                        }
                    } else {
                        MB_LOG_WARNING("Could not parse speed from CONNECT response");
                    }
                }

                MB_LOG_INFO("Hardware auto-answer sequence completed successfully");
                return SUCCESS;
            }

            /* Enhanced error detection during ringing phase */
            if (strstr(line_buf, "NO CARRIER") != NULL) {
                MB_LOG_ERROR("Connection lost during ringing phase: %s", line_buf);
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "BUSY") != NULL) {
                MB_LOG_ERROR("Line busy during ringing phase: %s", line_buf);
                return ERROR_MODEM;
            }
            if (strstr(line_buf, "ERROR") != NULL) {
                MB_LOG_ERROR("Modem error during ringing phase: %s", line_buf);
                return ERROR_MODEM;
            }
        }
    }

    return ERROR_TIMEOUT;
}

/**
 * Set DCD event callback for bridge integration
 * Allows Level 1 modem to notify Level 3 bridge of DCD state changes
 */
int modem_set_dcd_event_callback(modem_t *modem,
                                int (*callback)(void *user_data, bool dcd_state),
                                void *user_data)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Setting DCD event callback: %p (user_data: %p)",
                 (void *)callback, user_data);

    modem->dcd_event_callback = callback;
    modem->dcd_callback_user_data = user_data;

    return SUCCESS;
}
