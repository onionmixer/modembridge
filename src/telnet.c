/*
 * telnet.c - Telnet client protocol implementation
 */

#include "telnet.h"

/**
 * Initialize telnet structure
 */
void telnet_init(telnet_t *tn)
{
    if (tn == NULL) {
        return;
    }

    memset(tn, 0, sizeof(telnet_t));
    tn->fd = -1;
    tn->is_connected = false;
    tn->state = TELNET_STATE_DATA;

    /* Initialize option tracking */
    memset(tn->local_options, 0, sizeof(tn->local_options));
    memset(tn->remote_options, 0, sizeof(tn->remote_options));

    /* Set default options we support */
    tn->local_options[TELOPT_BINARY] = true;
    tn->local_options[TELOPT_SGA] = true;

    /* Default to line mode until server requests character mode */
    tn->linemode = true;

    /* Set default terminal type */
    SAFE_STRNCPY(tn->terminal_type, "ANSI", sizeof(tn->terminal_type));

    MB_LOG_DEBUG("Telnet initialized");
}

/**
 * Connect to telnet server
 */
int telnet_connect(telnet_t *tn, const char *host, int port)
{
    struct sockaddr_in server_addr;
    struct hostent *he;

    if (tn == NULL || host == NULL) {
        MB_LOG_ERROR("Invalid arguments to telnet_connect");
        return ERROR_INVALID_ARG;
    }

    if (tn->is_connected) {
        MB_LOG_WARNING("Already connected, disconnecting first");
        telnet_disconnect(tn);
    }

    MB_LOG_INFO("Connecting to telnet server: %s:%d", host, port);

    /* Create socket */
    tn->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tn->fd < 0) {
        MB_LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return ERROR_CONNECTION;
    }

    /* Set non-blocking mode */
    int flags = fcntl(tn->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(tn->fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Resolve hostname */
    he = gethostbyname(host);
    if (he == NULL) {
        MB_LOG_ERROR("Failed to resolve host: %s", host);
        close(tn->fd);
        tn->fd = -1;
        return ERROR_CONNECTION;
    }

    /* Setup server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    /* Connect to server */
    if (connect(tn->fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            MB_LOG_ERROR("Failed to connect: %s", strerror(errno));
            close(tn->fd);
            tn->fd = -1;
            return ERROR_CONNECTION;
        }
        /* Connection in progress for non-blocking socket */
    }

    /* Save connection info */
    SAFE_STRNCPY(tn->host, host, sizeof(tn->host));
    tn->port = port;
    tn->is_connected = true;

    MB_LOG_INFO("Connected to telnet server");

    /* Send initial option negotiations */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_SGA);
    telnet_send_negotiate(tn, TELNET_DO, TELOPT_SGA);
    telnet_send_negotiate(tn, TELNET_DO, TELOPT_ECHO);

    /* Offer TERMINAL-TYPE support (RFC 1091) */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_TTYPE);

    /* Offer LINEMODE support (RFC 1184) - character mode by default */
    telnet_send_negotiate(tn, TELNET_WILL, TELOPT_LINEMODE);

    return SUCCESS;
}

/**
 * Disconnect from telnet server
 */
int telnet_disconnect(telnet_t *tn)
{
    if (tn == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!tn->is_connected || tn->fd < 0) {
        return SUCCESS;
    }

    MB_LOG_INFO("Disconnecting from telnet server: %s:%d", tn->host, tn->port);

    close(tn->fd);
    tn->fd = -1;
    tn->is_connected = false;

    /* Reset state */
    tn->state = TELNET_STATE_DATA;
    tn->sb_len = 0;

    MB_LOG_INFO("Telnet disconnected");

    return SUCCESS;
}

/**
 * Send IAC command
 */
int telnet_send_command(telnet_t *tn, unsigned char command)
{
    unsigned char buf[2];

    if (tn == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    buf[0] = TELNET_IAC;
    buf[1] = command;

    MB_LOG_DEBUG("Sending IAC command: %d", command);

    if (send(tn->fd, buf, 2, 0) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            MB_LOG_ERROR("Failed to send IAC command: %s", strerror(errno));
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Send option negotiation
 */
int telnet_send_negotiate(telnet_t *tn, unsigned char command, unsigned char option)
{
    unsigned char buf[3];

    if (tn == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    buf[0] = TELNET_IAC;
    buf[1] = command;
    buf[2] = option;

    MB_LOG_DEBUG("Sending IAC negotiation: %d %d", command, option);

    if (send(tn->fd, buf, 3, 0) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            MB_LOG_ERROR("Failed to send negotiation: %s", strerror(errno));
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Update line mode vs character mode based on current options
 */
static void telnet_update_mode(telnet_t *tn)
{
    bool old_linemode;

    if (tn == NULL) {
        return;
    }

    old_linemode = tn->linemode;

    /* Update deprecated combined flags for compatibility */
    tn->binary_mode = tn->binary_local || tn->binary_remote;
    tn->sga_mode = tn->sga_local || tn->sga_remote;
    tn->echo_mode = tn->echo_remote;

    /* Character mode: Server echoes (WILL ECHO) and SGA enabled
     * Line mode: Client echoes (WONT ECHO) or no echo negotiation
     * LINEMODE overrides ECHO/SGA detection if active */
    if (tn->linemode_active) {
        tn->linemode = tn->linemode_edit;  /* LINEMODE MODE controls */
    } else if (tn->echo_remote && tn->sga_remote) {
        /* Character mode - server handles echo */
        tn->linemode = false;
        if (old_linemode != tn->linemode) {
            MB_LOG_INFO("Telnet mode: CHARACTER MODE (server echo, SGA enabled)");
        }
    } else {
        /* Line mode - client handles echo */
        tn->linemode = true;
        if (old_linemode != tn->linemode) {
            MB_LOG_INFO("Telnet mode: LINE MODE (client echo)");
        }
    }
}

/**
 * Handle received option negotiation (RFC 855 compliant with loop prevention)
 */
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option)
{
    if (tn == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Received IAC negotiation: cmd=%d opt=%d", command, option);

    switch (command) {
        case TELNET_WILL:
            /* Server will use option - only respond if state changes (RFC 855) */
            if (option == TELOPT_BINARY || option == TELOPT_SGA || option == TELOPT_ECHO) {
                if (!tn->remote_options[option]) {  /* State change check */
                    tn->remote_options[option] = true;
                    telnet_send_negotiate(tn, TELNET_DO, option);

                    if (option == TELOPT_BINARY) {
                        tn->binary_remote = true;
                        MB_LOG_INFO("Remote BINARY mode enabled");
                    } else if (option == TELOPT_SGA) {
                        tn->sga_remote = true;
                        MB_LOG_INFO("Remote SGA enabled");
                    } else if (option == TELOPT_ECHO) {
                        tn->echo_remote = true;
                        MB_LOG_INFO("Remote ECHO enabled");
                    }
                }
            } else {
                /* Reject unsupported options (only if not already rejected) */
                if (tn->remote_options[option]) {
                    tn->remote_options[option] = false;
                    telnet_send_negotiate(tn, TELNET_DONT, option);
                }
            }
            telnet_update_mode(tn);
            break;

        case TELNET_WONT:
            /* Server won't use option - only respond if state changes */
            if (tn->remote_options[option]) {
                tn->remote_options[option] = false;
                telnet_send_negotiate(tn, TELNET_DONT, option);

                if (option == TELOPT_BINARY) {
                    tn->binary_remote = false;
                } else if (option == TELOPT_SGA) {
                    tn->sga_remote = false;
                } else if (option == TELOPT_ECHO) {
                    tn->echo_remote = false;
                } else if (option == TELOPT_LINEMODE) {
                    tn->linemode_active = false;
                }
            }
            telnet_update_mode(tn);
            break;

        case TELNET_DO:
            /* Server wants us to use option - only respond if state changes */
            if (option == TELOPT_BINARY || option == TELOPT_SGA ||
                option == TELOPT_TTYPE || option == TELOPT_LINEMODE) {
                if (!tn->local_options[option]) {  /* State change check */
                    tn->local_options[option] = true;
                    telnet_send_negotiate(tn, TELNET_WILL, option);

                    if (option == TELOPT_BINARY) {
                        tn->binary_local = true;
                        MB_LOG_INFO("Local BINARY mode enabled");
                    } else if (option == TELOPT_SGA) {
                        tn->sga_local = true;
                        MB_LOG_INFO("Local SGA enabled");
                    } else if (option == TELOPT_TTYPE) {
                        MB_LOG_INFO("TERMINAL-TYPE negotiation accepted");
                        /* Server will send SB TTYPE SEND to request type */
                    } else if (option == TELOPT_LINEMODE) {
                        tn->linemode_active = true;
                        MB_LOG_INFO("LINEMODE negotiation accepted");
                        /* Server may send MODE subnegotiation */
                    }
                }
            } else {
                /* Reject unsupported options (only if not already rejected) */
                if (tn->local_options[option]) {
                    tn->local_options[option] = false;
                    telnet_send_negotiate(tn, TELNET_WONT, option);
                }
            }
            telnet_update_mode(tn);
            break;

        case TELNET_DONT:
            /* Server doesn't want us to use option - only respond if state changes */
            if (tn->local_options[option]) {
                tn->local_options[option] = false;
                telnet_send_negotiate(tn, TELNET_WONT, option);

                if (option == TELOPT_BINARY) {
                    tn->binary_local = false;
                } else if (option == TELOPT_SGA) {
                    tn->sga_local = false;
                } else if (option == TELOPT_LINEMODE) {
                    tn->linemode_active = false;
                }
            }
            telnet_update_mode(tn);
            break;

        default:
            MB_LOG_WARNING("Unknown negotiation command: %d", command);
            break;
    }

    return SUCCESS;
}

/**
 * Send subnegotiation (helper function)
 */
static int telnet_send_subnegotiation(telnet_t *tn, const unsigned char *data, size_t len)
{
    unsigned char buf[BUFFER_SIZE];
    size_t pos = 0;

    if (tn == NULL || data == NULL || len == 0 || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    /* Build: IAC SB <data...> IAC SE */
    buf[pos++] = TELNET_IAC;
    buf[pos++] = TELNET_SB;

    for (size_t i = 0; i < len && pos < sizeof(buf) - 2; i++) {
        /* Escape IAC in subnegotiation data (RFC 854) */
        if (data[i] == TELNET_IAC) {
            buf[pos++] = TELNET_IAC;
            buf[pos++] = TELNET_IAC;
        } else {
            buf[pos++] = data[i];
        }
    }

    buf[pos++] = TELNET_IAC;
    buf[pos++] = TELNET_SE;

    MB_LOG_DEBUG("Sending subnegotiation: %zu bytes", pos);

    if (send(tn->fd, buf, pos, 0) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            MB_LOG_ERROR("Failed to send subnegotiation: %s", strerror(errno));
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

/**
 * Handle subnegotiation (RFC 1091 TERMINAL-TYPE, RFC 1184 LINEMODE)
 */
int telnet_handle_subnegotiation(telnet_t *tn)
{
    if (tn == NULL || tn->sb_len < 1) {
        return ERROR_INVALID_ARG;
    }

    unsigned char option = tn->sb_buffer[0];

    MB_LOG_DEBUG("Received subnegotiation for option %d, length %zu", (int)option, tn->sb_len);

    switch (option) {
        case TELOPT_TTYPE:
            /* TERMINAL-TYPE subnegotiation (RFC 1091) */
            if (tn->sb_len >= 2 && tn->sb_buffer[1] == TTYPE_SEND) {
                /* Server requests terminal type - send IS response */
                unsigned char response[68];  /* 1 (option) + 1 (IS) + 64 (terminal type) + 2 safety */
                size_t term_len = strlen(tn->terminal_type);

                response[0] = TELOPT_TTYPE;
                response[1] = TTYPE_IS;
                memcpy(&response[2], tn->terminal_type, term_len);

                MB_LOG_INFO("Sending TERMINAL-TYPE IS %s", tn->terminal_type);
                telnet_send_subnegotiation(tn, response, 2 + term_len);
            }
            break;

        case TELOPT_LINEMODE:
            /* LINEMODE subnegotiation (RFC 1184) */
            if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_MODE) {
                /* MODE subnegotiation */
                if (tn->sb_len >= 3) {
                    unsigned char mode = tn->sb_buffer[2];
                    bool old_edit = tn->linemode_edit;

                    tn->linemode_edit = (mode & MODE_EDIT) != 0;

                    MB_LOG_INFO("LINEMODE MODE: EDIT=%s TRAPSIG=%s",
                               (mode & MODE_EDIT) ? "yes" : "no",
                               (mode & MODE_TRAPSIG) ? "yes" : "no");

                    /* Send ACK if MODE_ACK bit is set (RFC 1184 mode synchronization) */
                    if (mode & MODE_ACK) {
                        unsigned char response[3];
                        response[0] = TELOPT_LINEMODE;
                        response[1] = LM_MODE;
                        response[2] = mode;  /* Echo back the same mode */

                        MB_LOG_DEBUG("Sending LINEMODE MODE ACK");
                        telnet_send_subnegotiation(tn, response, 3);
                    }

                    /* Update mode if edit flag changed */
                    if (old_edit != tn->linemode_edit) {
                        telnet_update_mode(tn);
                    }
                }
            } else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_FORWARDMASK) {
                /* FORWARDMASK - acknowledge but don't implement for now */
                MB_LOG_DEBUG("Received LINEMODE FORWARDMASK (not implemented)");
            } else if (tn->sb_len >= 2 && tn->sb_buffer[1] == LM_SLC) {
                /* SLC (Set Local Characters) - acknowledge but don't implement for now */
                MB_LOG_DEBUG("Received LINEMODE SLC (not implemented)");
            }
            break;

        default:
            /* Unknown option - just log and ignore */
            MB_LOG_DEBUG("Ignoring subnegotiation for unsupported option %d", option);
            break;
    }

    return SUCCESS;
}

/**
 * Process incoming data from telnet server
 */
int telnet_process_input(telnet_t *tn, const unsigned char *input, size_t input_len,
                         unsigned char *output, size_t output_size, size_t *output_len)
{
    size_t out_pos = 0;

    if (tn == NULL || input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    *output_len = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];

        switch (tn->state) {
            case TELNET_STATE_DATA:
                if (c == TELNET_IAC) {
                    tn->state = TELNET_STATE_IAC;
                } else {
                    /* Regular data */
                    if (out_pos < output_size) {
                        output[out_pos++] = c;
                    }
                }
                break;

            case TELNET_STATE_IAC:
                if (c == TELNET_IAC) {
                    /* Escaped IAC - output single IAC */
                    if (out_pos < output_size) {
                        output[out_pos++] = TELNET_IAC;
                    }
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_WILL) {
                    tn->state = TELNET_STATE_WILL;
                } else if (c == TELNET_WONT) {
                    tn->state = TELNET_STATE_WONT;
                } else if (c == TELNET_DO) {
                    tn->state = TELNET_STATE_DO;
                } else if (c == TELNET_DONT) {
                    tn->state = TELNET_STATE_DONT;
                } else if (c == TELNET_SB) {
                    tn->state = TELNET_STATE_SB;
                    tn->sb_len = 0;
                } else if (c == TELNET_GA) {
                    /* Go Ahead - silently ignore in character mode (RFC 858) */
                    MB_LOG_DEBUG("Received IAC GA (ignored)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_NOP) {
                    /* No Operation - silently ignore (RFC 854) */
                    MB_LOG_DEBUG("Received IAC NOP");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_AYT) {
                    /* Are You There - respond with confirmation (RFC 854) */
                    MB_LOG_DEBUG("Received IAC AYT");
                    const char *response = "\r\n[ModemBridge: Yes, I'm here]\r\n";
                    telnet_send(tn, response, strlen(response));
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_IP) {
                    /* Interrupt Process - log but don't act (RFC 854) */
                    MB_LOG_INFO("Received IAC IP (Interrupt Process)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_AO) {
                    /* Abort Output - log but don't act (RFC 854) */
                    MB_LOG_INFO("Received IAC AO (Abort Output)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_BREAK) {
                    /* Break - log but don't act (RFC 854) */
                    MB_LOG_INFO("Received IAC BREAK");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_EL) {
                    /* Erase Line - log but don't act (RFC 854) */
                    MB_LOG_DEBUG("Received IAC EL (Erase Line)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_EC) {
                    /* Erase Character - log but don't act (RFC 854) */
                    MB_LOG_DEBUG("Received IAC EC (Erase Character)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_DM) {
                    /* Data Mark - marks end of urgent data (RFC 854) */
                    MB_LOG_DEBUG("Received IAC DM (Data Mark)");
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_EOR) {
                    /* End of Record - log but don't act (RFC 885) */
                    MB_LOG_DEBUG("Received IAC EOR (End of Record)");
                    tn->state = TELNET_STATE_DATA;
                } else {
                    /* Unknown IAC command - log and ignore */
                    MB_LOG_WARNING("Received unknown IAC command: %d", c);
                    tn->state = TELNET_STATE_DATA;
                }
                break;

            case TELNET_STATE_WILL:
                telnet_handle_negotiate(tn, TELNET_WILL, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_WONT:
                telnet_handle_negotiate(tn, TELNET_WONT, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_DO:
                telnet_handle_negotiate(tn, TELNET_DO, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_DONT:
                telnet_handle_negotiate(tn, TELNET_DONT, c);
                tn->state = TELNET_STATE_DATA;
                break;

            case TELNET_STATE_SB:
                if (c == TELNET_IAC) {
                    tn->state = TELNET_STATE_SB_IAC;
                } else {
                    /* Accumulate subnegotiation data */
                    if (tn->sb_len < sizeof(tn->sb_buffer)) {
                        tn->sb_buffer[tn->sb_len++] = c;
                    }
                }
                break;

            case TELNET_STATE_SB_IAC:
                if (c == TELNET_SE) {
                    /* End of subnegotiation */
                    telnet_handle_subnegotiation(tn);
                    tn->sb_len = 0;
                    tn->state = TELNET_STATE_DATA;
                } else if (c == TELNET_IAC) {
                    /* Escaped IAC in subnegotiation */
                    if (tn->sb_len < sizeof(tn->sb_buffer)) {
                        tn->sb_buffer[tn->sb_len++] = TELNET_IAC;
                    }
                    tn->state = TELNET_STATE_SB;
                } else {
                    /* Invalid sequence - return to SB state */
                    if (tn->sb_len < sizeof(tn->sb_buffer)) {
                        tn->sb_buffer[tn->sb_len++] = c;
                    }
                    tn->state = TELNET_STATE_SB;
                }
                break;

            default:
                MB_LOG_WARNING("Invalid telnet state: %d", tn->state);
                tn->state = TELNET_STATE_DATA;
                break;
        }
    }

    *output_len = out_pos;

    if (out_pos > 0) {
        MB_LOG_DEBUG("Telnet processed %zu bytes -> %zu bytes", input_len, out_pos);
    }

    return SUCCESS;
}

/**
 * Prepare data for sending to telnet server (escape IAC bytes)
 */
int telnet_prepare_output(telnet_t *tn, const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_size, size_t *output_len)
{
    size_t out_pos = 0;

    if (tn == NULL || input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    *output_len = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];

        if (c == TELNET_IAC) {
            /* Escape IAC by doubling it */
            if (out_pos + 1 < output_size) {
                output[out_pos++] = TELNET_IAC;
                output[out_pos++] = TELNET_IAC;
            } else {
                /* Output buffer full */
                break;
            }
        } else {
            /* Regular character */
            if (out_pos < output_size) {
                output[out_pos++] = c;
            } else {
                /* Output buffer full */
                break;
            }
        }
    }

    *output_len = out_pos;

    if (out_pos > 0) {
        MB_LOG_DEBUG("Telnet prepared %zu bytes -> %zu bytes", input_len, out_pos);
    }

    return SUCCESS;
}

/**
 * Send data to telnet server
 */
ssize_t telnet_send(telnet_t *tn, const void *data, size_t len)
{
    ssize_t sent;

    if (tn == NULL || data == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!tn->is_connected) {
        return ERROR_CONNECTION;
    }

    MB_LOG_DEBUG("Telnet sending %zu bytes", len);

    sent = send(tn->fd, data, len, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Would block */
            return 0;
        }
        MB_LOG_ERROR("Telnet send error: %s", strerror(errno));
        return ERROR_IO;
    }

    return sent;
}

/**
 * Receive data from telnet server
 */
ssize_t telnet_recv(telnet_t *tn, void *buffer, size_t size)
{
    ssize_t n;

    if (tn == NULL || buffer == NULL || tn->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!tn->is_connected) {
        return ERROR_CONNECTION;
    }

    n = recv(tn->fd, buffer, size, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No data available */
            return 0;
        }
        MB_LOG_ERROR("Telnet recv error: %s", strerror(errno));
        return ERROR_IO;
    }

    if (n == 0) {
        /* Connection closed */
        MB_LOG_INFO("Telnet connection closed by server");
        tn->is_connected = false;
        return 0;
    }

    MB_LOG_DEBUG("Telnet received %zd bytes", n);

    return n;
}

/**
 * Get file descriptor for select/poll
 */
int telnet_get_fd(telnet_t *tn)
{
    if (tn == NULL) {
        return -1;
    }

    return tn->fd;
}

/**
 * Check if connected to telnet server
 */
bool telnet_is_connected(telnet_t *tn)
{
    if (tn == NULL) {
        return false;
    }

    return tn->is_connected && tn->fd >= 0;
}

/**
 * Check if in line mode
 */
bool telnet_is_linemode(telnet_t *tn)
{
    if (tn == NULL) {
        return true;
    }

    return tn->linemode;
}

/**
 * Check if in binary mode
 */
bool telnet_is_binary_mode(telnet_t *tn)
{
    if (tn == NULL) {
        return false;
    }

    return tn->binary_mode;
}
