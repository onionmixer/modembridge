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
    modem->settings.s_registers[SREG_AUTO_ANSWER] = 0;      /* No auto-answer */
    modem->settings.s_registers[SREG_RING_COUNT] = 0;       /* Ring counter */
    modem->settings.s_registers[SREG_ESCAPE_CHAR] = '+';    /* Escape character */
    modem->settings.s_registers[SREG_CR_CHAR] = '\r';       /* CR character */
    modem->settings.s_registers[SREG_LF_CHAR] = '\n';       /* LF character */
    modem->settings.s_registers[SREG_BS_CHAR] = '\b';       /* Backspace character */

    /* Clear command buffer */
    memset(modem->cmd_buffer, 0, sizeof(modem->cmd_buffer));
    modem->cmd_len = 0;

    /* Reset escape sequence detection */
    modem->escape_count = 0;
    modem->last_escape_time = 0;

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
    int len;

    if (modem == NULL || response == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (modem->settings.quiet) {
        /* Quiet mode - no responses */
        return SUCCESS;
    }

    /* Format response based on verbose mode */
    if (modem->settings.verbose) {
        /* Verbose mode: \r\nRESPONSE\r\n */
        len = snprintf(buf, sizeof(buf), "\r\n%s\r\n", response);
    } else {
        /* Numeric mode: just the code */
        /* Map response to numeric code */
        int code = 0;
        if (strcmp(response, MODEM_RESP_OK) == 0) code = 0;
        else if (strcmp(response, MODEM_RESP_CONNECT) == 0) code = 1;
        else if (strcmp(response, MODEM_RESP_RING) == 0) code = 2;
        else if (strcmp(response, MODEM_RESP_NO_CARRIER) == 0) code = 3;
        else if (strcmp(response, MODEM_RESP_ERROR) == 0) code = 4;
        else if (strcmp(response, MODEM_RESP_NO_DIALTONE) == 0) code = 6;
        else if (strcmp(response, MODEM_RESP_BUSY) == 0) code = 7;
        else if (strcmp(response, MODEM_RESP_NO_ANSWER) == 0) code = 8;

        len = snprintf(buf, sizeof(buf), "\r\n%d\r\n", code);
    }

    if (len >= (int)sizeof(buf)) {
        MB_LOG_WARNING("Response truncated");
        len = sizeof(buf) - 1;
    }

    MB_LOG_DEBUG("Modem response: %s", response);

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
 */
int modem_hangup(modem_t *modem)
{
    if (modem == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Hanging up");

    modem->state = MODEM_STATE_DISCONNECTED;
    modem->online = false;
    modem->carrier = false;

    /* Clear DTR */
    serial_set_dtr(modem->serial, false);

    /* Reset ring counter */
    modem->settings.s_registers[SREG_RING_COUNT] = 0;

    return SUCCESS;
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
            /* AT&F - Factory defaults */
            if (*p == 'F') {
                MB_LOG_INFO("AT command: AT&F (Factory defaults)");
                p++;
                modem_reset(modem);
            }
            else {
                /* Skip unsupported & command */
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
 * Process incoming data from serial port
 */
ssize_t modem_process_input(modem_t *modem, const char *data, size_t len)
{
    size_t consumed = 0;
    time_t now = time(NULL);

    if (modem == NULL || data == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* In online mode, check for escape sequence */
    if (modem->online) {
        int escape_char = modem->settings.s_registers[SREG_ESCAPE_CHAR];

        for (size_t i = 0; i < len; i++) {
            if (data[i] == escape_char) {
                /* Check guard time */
                if (modem->escape_count == 0 ||
                    (now - modem->last_escape_time) <= 2) {
                    modem->escape_count++;
                    modem->last_escape_time = now;

                    if (modem->escape_count >= 3) {
                        /* Escape sequence detected - go to command mode */
                        MB_LOG_INFO("Escape sequence detected (+++), entering command mode");
                        modem_go_offline(modem);
                        modem_send_response(modem, MODEM_RESP_OK);
                        consumed = i + 1;
                        return consumed;
                    }
                } else {
                    /* Guard time violated - reset */
                    modem->escape_count = 1;
                    modem->last_escape_time = now;
                }
            } else {
                /* Non-escape character - reset counter */
                modem->escape_count = 0;
            }
        }

        /* In online mode, data passes through */
        return len;
    }

    /* In command mode, process AT commands */
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        int cr_char = modem->settings.s_registers[SREG_CR_CHAR];
        int bs_char = modem->settings.s_registers[SREG_BS_CHAR];

        /* Echo if enabled */
        if (modem->settings.echo && c != '\0') {
            serial_write(modem->serial, &c, 1);
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
            consumed++;
            continue;
        }

        /* Handle carriage return - execute command */
        if (c == cr_char || c == '\n') {
            consumed++;

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

        consumed++;
    }

    return consumed;
}
