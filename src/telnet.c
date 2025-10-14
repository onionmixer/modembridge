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

    /* Character mode: Server echoes (WILL ECHO) and SGA enabled
     * Line mode: Client echoes (WONT ECHO) or no echo negotiation */
    if (tn->remote_options[TELOPT_ECHO] && tn->remote_options[TELOPT_SGA]) {
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
 * Handle received option negotiation
 */
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option)
{
    if (tn == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Received IAC negotiation: %d %d", command, option);

    switch (command) {
        case TELNET_WILL:
            /* Server will use option */
            if (option == TELOPT_BINARY || option == TELOPT_SGA || option == TELOPT_ECHO) {
                /* Accept these options */
                tn->remote_options[option] = true;
                telnet_send_negotiate(tn, TELNET_DO, option);

                if (option == TELOPT_BINARY) {
                    tn->binary_mode = true;
                    MB_LOG_INFO("Binary mode enabled");
                } else if (option == TELOPT_SGA) {
                    tn->sga_mode = true;
                    MB_LOG_INFO("SGA mode enabled");
                } else if (option == TELOPT_ECHO) {
                    tn->echo_mode = true;
                    MB_LOG_INFO("Echo mode enabled");
                }
            } else {
                /* Reject other options */
                telnet_send_negotiate(tn, TELNET_DONT, option);
            }
            /* Update line/character mode after option change */
            telnet_update_mode(tn);
            break;

        case TELNET_WONT:
            /* Server won't use option */
            tn->remote_options[option] = false;
            /* Acknowledge with DONT */
            telnet_send_negotiate(tn, TELNET_DONT, option);

            if (option == TELOPT_BINARY) {
                tn->binary_mode = false;
            } else if (option == TELOPT_SGA) {
                tn->sga_mode = false;
            } else if (option == TELOPT_ECHO) {
                tn->echo_mode = false;
            }
            /* Update line/character mode after option change */
            telnet_update_mode(tn);
            break;

        case TELNET_DO:
            /* Server wants us to use option */
            if (option == TELOPT_BINARY || option == TELOPT_SGA) {
                /* Accept these options */
                tn->local_options[option] = true;
                telnet_send_negotiate(tn, TELNET_WILL, option);

                if (option == TELOPT_BINARY) {
                    tn->binary_mode = true;
                    MB_LOG_INFO("Binary mode enabled (local)");
                } else if (option == TELOPT_SGA) {
                    tn->sga_mode = true;
                    MB_LOG_INFO("SGA mode enabled (local)");
                }
            } else {
                /* Reject other options */
                telnet_send_negotiate(tn, TELNET_WONT, option);
            }
            /* Update line/character mode after option change */
            telnet_update_mode(tn);
            break;

        case TELNET_DONT:
            /* Server doesn't want us to use option */
            tn->local_options[option] = false;
            /* Acknowledge with WONT */
            telnet_send_negotiate(tn, TELNET_WONT, option);

            if (option == TELOPT_BINARY) {
                tn->binary_mode = false;
            } else if (option == TELOPT_SGA) {
                tn->sga_mode = false;
            }
            /* Update line/character mode after option change */
            telnet_update_mode(tn);
            break;

        default:
            MB_LOG_WARNING("Unknown negotiation command: %d", command);
            break;
    }

    return SUCCESS;
}

/**
 * Handle subnegotiation
 */
int telnet_handle_subnegotiation(telnet_t *tn)
{
    if (tn == NULL || tn->sb_len < 1) {
        return ERROR_INVALID_ARG;
    }

    unsigned char option = tn->sb_buffer[0];

    MB_LOG_DEBUG("Received subnegotiation for option %d, length %zu", (int)option, tn->sb_len);

    /* Handle specific subnegotiations if needed */
    /* For now, we just log and ignore */
    (void)option;  /* Mark as intentionally unused for now */

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
                } else {
                    /* Other IAC commands - just log */
                    MB_LOG_DEBUG("Received IAC command: %d", c);
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
