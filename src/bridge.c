/*
 * bridge.c - Main bridging logic for ModemBridge
 */

#include "bridge.h"
#include <sys/select.h>
#include <time.h>

/* Forward declarations */
static int bridge_transfer_telnet_to_serial(bridge_ctx_t *ctx);

/**
 * Initialize circular buffer
 */
void cbuf_init(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return;
    }

    memset(buf, 0, sizeof(circular_buffer_t));
    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->count = 0;
}

/**
 * Write data to circular buffer
 */
size_t cbuf_write(circular_buffer_t *buf, const unsigned char *data, size_t len)
{
    size_t written = 0;

    if (buf == NULL || data == NULL) {
        return 0;
    }

    while (written < len && buf->count < BUFFER_SIZE) {
        buf->data[buf->write_pos] = data[written];
        buf->write_pos = (buf->write_pos + 1) % BUFFER_SIZE;
        buf->count++;
        written++;
    }

    return written;
}

/**
 * Read data from circular buffer
 */
size_t cbuf_read(circular_buffer_t *buf, unsigned char *data, size_t len)
{
    size_t read_count = 0;

    if (buf == NULL || data == NULL) {
        return 0;
    }

    while (read_count < len && buf->count > 0) {
        data[read_count] = buf->data[buf->read_pos];
        buf->read_pos = (buf->read_pos + 1) % BUFFER_SIZE;
        buf->count--;
        read_count++;
    }

    return read_count;
}

/**
 * Get available data in buffer
 */
size_t cbuf_available(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return 0;
    }

    return buf->count;
}

/**
 * Get free space in buffer
 */
size_t cbuf_free(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return 0;
    }

    return BUFFER_SIZE - buf->count;
}

/**
 * Check if buffer is empty
 */
bool cbuf_is_empty(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return true;
    }

    return buf->count == 0;
}

/**
 * Check if buffer is full
 */
bool cbuf_is_full(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return false;
    }

    return buf->count >= BUFFER_SIZE;
}

/**
 * Clear circular buffer
 */
void cbuf_clear(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return;
    }

    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->count = 0;
}

/**
 * Check if byte is start of multibyte UTF-8 sequence
 */
bool is_utf8_start(unsigned char byte)
{
    /* UTF-8 start bytes: 11xxxxxx */
    return (byte & 0xC0) == 0xC0 && (byte & 0xFE) != 0xFE;
}

/**
 * Check if byte is UTF-8 continuation byte
 */
bool is_utf8_continuation(unsigned char byte)
{
    /* UTF-8 continuation bytes: 10xxxxxx */
    return (byte & 0xC0) == 0x80;
}

/**
 * Get expected length of UTF-8 sequence from first byte
 */
int utf8_sequence_length(unsigned char byte)
{
    if ((byte & 0x80) == 0x00) {
        /* 0xxxxxxx - 1 byte (ASCII) */
        return 1;
    } else if ((byte & 0xE0) == 0xC0) {
        /* 110xxxxx - 2 bytes */
        return 2;
    } else if ((byte & 0xF0) == 0xE0) {
        /* 1110xxxx - 3 bytes */
        return 3;
    } else if ((byte & 0xF8) == 0xF0) {
        /* 11110xxx - 4 bytes */
        return 4;
    }

    /* Invalid */
    return 0;
}

/**
 * Validate complete UTF-8 sequence
 */
bool is_valid_utf8_sequence(const unsigned char *seq, size_t len)
{
    if (seq == NULL || len == 0) {
        return false;
    }

    int expected_len = utf8_sequence_length(seq[0]);
    if (expected_len == 0 || (size_t)expected_len != len) {
        return false;
    }

    /* Check continuation bytes */
    for (size_t i = 1; i < len; i++) {
        if (!is_utf8_continuation(seq[i])) {
            return false;
        }
    }

    return true;
}

/**
 * Filter ANSI escape sequences from modem input
 */
int ansi_filter_modem_to_telnet(const unsigned char *input, size_t input_len,
                                unsigned char *output, size_t output_size,
                                size_t *output_len, ansi_state_t *state)
{
    size_t out_pos = 0;
    ansi_state_t current_state = state ? *state : ANSI_STATE_NORMAL;

    if (input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    *output_len = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];

        switch (current_state) {
            case ANSI_STATE_NORMAL:
                if (c == 0x1B) {  /* ESC */
                    current_state = ANSI_STATE_ESC;
                } else {
                    /* Normal character - pass through */
                    if (out_pos < output_size) {
                        output[out_pos++] = c;
                    }
                }
                break;

            case ANSI_STATE_ESC:
                if (c == '[') {
                    /* CSI sequence */
                    current_state = ANSI_STATE_CSI;
                } else if (c == 'c') {
                    /* Reset - filter out */
                    current_state = ANSI_STATE_NORMAL;
                } else {
                    /* Other escape sequences - filter out for now */
                    current_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_CSI:
                /* Check if this is a parameter or intermediate byte */
                if (c >= 0x30 && c <= 0x3F) {
                    /* Parameter bytes (0-9:;<=>?) */
                    current_state = ANSI_STATE_CSI_PARAM;
                } else if (c >= 0x40 && c <= 0x7E) {
                    /* Final byte - end of CSI sequence */
                    current_state = ANSI_STATE_NORMAL;
                } else {
                    /* Invalid - return to normal */
                    current_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_CSI_PARAM:
                if (c >= 0x30 && c <= 0x3F) {
                    /* More parameter bytes */
                    /* Stay in CSI_PARAM state */
                } else if (c >= 0x40 && c <= 0x7E) {
                    /* Final byte - end of CSI sequence */
                    current_state = ANSI_STATE_NORMAL;
                } else {
                    /* Invalid - return to normal */
                    current_state = ANSI_STATE_NORMAL;
                }
                break;

            default:
                current_state = ANSI_STATE_NORMAL;
                break;
        }
    }

    *output_len = out_pos;

    if (state) {
        *state = current_state;
    }

    return SUCCESS;
}

/**
 * Pass through ANSI escape sequences from telnet to modem
 */
int ansi_passthrough_telnet_to_modem(const unsigned char *input, size_t input_len,
                                     unsigned char *output, size_t output_size,
                                     size_t *output_len)
{
    if (input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Simple passthrough - just copy data */
    size_t copy_len = MIN(input_len, output_size);
    memcpy(output, input, copy_len);
    *output_len = copy_len;

    return SUCCESS;
}

/**
 * Initialize bridge context
 */
void bridge_init(bridge_ctx_t *ctx, config_t *cfg)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(bridge_ctx_t));

    ctx->config = cfg;
    ctx->state = STATE_IDLE;
    ctx->running = false;

    /* Initialize components */
    serial_init(&ctx->serial);
    telnet_init(&ctx->telnet);

    /* Initialize buffers */
    cbuf_init(&ctx->serial_to_telnet_buf);
    cbuf_init(&ctx->telnet_to_serial_buf);

    /* Initialize ANSI filter state */
    ctx->ansi_filter_state = ANSI_STATE_NORMAL;

    /* Initialize statistics */
    ctx->bytes_serial_to_telnet = 0;
    ctx->bytes_telnet_to_serial = 0;
    ctx->connection_start_time = 0;

    MB_LOG_DEBUG("Bridge context initialized");
}

/**
 * Start bridge operation
 */
int bridge_start(bridge_ctx_t *ctx)
{
    if (ctx == NULL || ctx->config == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Starting bridge");

    /* Open serial port */
    int ret = serial_open(&ctx->serial, ctx->config->comport, ctx->config);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to open serial port");
        return ret;
    }

    /* Initialize modem */
    modem_init(&ctx->modem, &ctx->serial);

    ctx->state = STATE_IDLE;
    ctx->running = true;

    MB_LOG_INFO("Bridge started, waiting for modem connection");

    return SUCCESS;
}

/**
 * Stop bridge operation
 */
int bridge_stop(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Stopping bridge");

    ctx->running = false;

    /* Disconnect telnet if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }

    /* Hang up modem */
    if (modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
    }

    /* Close serial port */
    serial_close(&ctx->serial);

    /* Print statistics */
    bridge_print_stats(ctx);

    MB_LOG_INFO("Bridge stopped");

    return SUCCESS;
}

/**
 * Handle modem connection establishment
 */
int bridge_handle_modem_connect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Modem connection established, connecting to telnet server");

    /* Connect to telnet server */
    int ret = telnet_connect(&ctx->telnet, ctx->config->telnet_host, ctx->config->telnet_port);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to connect to telnet server");
        modem_send_response(&ctx->modem, MODEM_RESP_NO_CARRIER);
        ctx->state = STATE_IDLE;
        return ret;
    }

    /* Go online */
    modem_go_online(&ctx->modem);

    /* Send CONNECT message */
    modem_send_connect(&ctx->modem, ctx->config->baudrate_value);

    ctx->state = STATE_CONNECTED;
    ctx->connection_start_time = time(NULL);

    MB_LOG_INFO("Bridge connection established");

    return SUCCESS;
}

/**
 * Handle modem disconnection
 */
int bridge_handle_modem_disconnect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Modem disconnected");

    /* Disconnect telnet */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }

    /* Send NO CARRIER */
    modem_send_no_carrier(&ctx->modem);

    ctx->state = STATE_IDLE;

    return SUCCESS;
}

/**
 * Handle telnet connection establishment
 */
int bridge_handle_telnet_connect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Telnet connection established");

    ctx->state = STATE_CONNECTED;

    return SUCCESS;
}

/**
 * Handle telnet disconnection
 */
int bridge_handle_telnet_disconnect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Telnet disconnected");

    /* Hang up modem */
    if (modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
        modem_send_no_carrier(&ctx->modem);
    }

    ctx->state = STATE_IDLE;

    return SUCCESS;
}

/**
 * Process data from serial port
 */
int bridge_process_serial_data(bridge_ctx_t *ctx)
{
    unsigned char buf[BUFFER_SIZE];
    unsigned char filtered_buf[BUFFER_SIZE];
    unsigned char telnet_buf[BUFFER_SIZE * 2];
    size_t filtered_len, telnet_len;
    ssize_t n;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Read from serial port */
    n = serial_read(&ctx->serial, buf, sizeof(buf));
    if (n < 0) {
        MB_LOG_ERROR("Serial read error");
        return ERROR_IO;
    }

    if (n == 0) {
        /* No data */
        return SUCCESS;
    }

    /* Process through modem layer */
    if (!modem_is_online(&ctx->modem)) {
        /* In command mode - let modem process the input */
        modem_process_input(&ctx->modem, (char *)buf, n);

        /* Check for state changes */
        if (modem_get_state(&ctx->modem) == MODEM_STATE_CONNECTING) {
            bridge_handle_modem_connect(ctx);
        } else if (modem_get_state(&ctx->modem) == MODEM_STATE_DISCONNECTED) {
            bridge_handle_modem_disconnect(ctx);
        }

        return SUCCESS;
    }

    /* Online mode - check for escape sequence and forward data */
    ssize_t consumed = modem_process_input(&ctx->modem, (char *)buf, n);
    if (!modem_is_online(&ctx->modem)) {
        /* Modem went offline (escape sequence detected) */
        return SUCCESS;
    }

    if (consumed <= 0) {
        /* No data to transfer */
        return SUCCESS;
    }

    /* Transfer actual data to telnet */
    if (!telnet_is_connected(&ctx->telnet)) {
        return SUCCESS;
    }

    /* Filter ANSI sequences */
    ansi_filter_modem_to_telnet(buf, consumed, filtered_buf, sizeof(filtered_buf),
                                &filtered_len, &ctx->ansi_filter_state);

    if (filtered_len == 0) {
        return SUCCESS;
    }

    /* Prepare for telnet (escape IAC) */
    telnet_prepare_output(&ctx->telnet, filtered_buf, filtered_len,
                         telnet_buf, sizeof(telnet_buf), &telnet_len);

    if (telnet_len == 0) {
        return SUCCESS;
    }

    /* Send to telnet */
    ssize_t sent = telnet_send(&ctx->telnet, telnet_buf, telnet_len);
    if (sent > 0) {
        ctx->bytes_serial_to_telnet += sent;
    }

    return SUCCESS;
}

/**
 * Process data from telnet
 */
int bridge_process_telnet_data(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Transfer data from telnet to serial */
    if (telnet_is_connected(&ctx->telnet) && modem_is_online(&ctx->modem)) {
        return bridge_transfer_telnet_to_serial(ctx);
    }

    return SUCCESS;
}

/**
 * Transfer data from telnet to serial
 */
static int bridge_transfer_telnet_to_serial(bridge_ctx_t *ctx)
{
    unsigned char telnet_buf[BUFFER_SIZE];
    unsigned char processed_buf[BUFFER_SIZE];
    unsigned char output_buf[BUFFER_SIZE];
    size_t processed_len, output_len;
    ssize_t n;

    /* Read from telnet */
    n = telnet_recv(&ctx->telnet, telnet_buf, sizeof(telnet_buf));
    if (n < 0) {
        MB_LOG_ERROR("Telnet connection error");
        bridge_handle_telnet_disconnect(ctx);
        return ERROR_CONNECTION;
    }

    if (n == 0) {
        /* Check if connection closed */
        if (!telnet_is_connected(&ctx->telnet)) {
            bridge_handle_telnet_disconnect(ctx);
            return ERROR_CONNECTION;
        }
        return SUCCESS;
    }

    /* Process telnet protocol (remove IAC sequences) */
    telnet_process_input(&ctx->telnet, telnet_buf, n,
                        processed_buf, sizeof(processed_buf), &processed_len);

    if (processed_len == 0) {
        return SUCCESS;
    }

    /* Pass through ANSI sequences to modem client */
    ansi_passthrough_telnet_to_modem(processed_buf, processed_len,
                                    output_buf, sizeof(output_buf), &output_len);

    if (output_len == 0) {
        return SUCCESS;
    }

    /* Send to serial */
    ssize_t sent = serial_write(&ctx->serial, output_buf, output_len);
    if (sent > 0) {
        ctx->bytes_telnet_to_serial += sent;
    }

    return SUCCESS;
}

/**
 * Main bridge loop (I/O multiplexing)
 */
int bridge_run(bridge_ctx_t *ctx)
{
    fd_set readfds;
    struct timeval timeout;
    int maxfd = 0;
    int ret;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->running) {
        return ERROR_GENERAL;
    }

    /* Setup file descriptor set */
    FD_ZERO(&readfds);

    /* Add serial port */
    int serial_fd = serial_get_fd(&ctx->serial);
    if (serial_fd >= 0) {
        FD_SET(serial_fd, &readfds);
        maxfd = MAX(maxfd, serial_fd);
    }

    /* Add telnet socket if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        int telnet_fd = telnet_get_fd(&ctx->telnet);
        if (telnet_fd >= 0) {
            FD_SET(telnet_fd, &readfds);
            maxfd = MAX(maxfd, telnet_fd);
        }
    }

    /* Set timeout */
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    /* Wait for activity */
    ret = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
    if (ret < 0) {
        if (errno == EINTR) {
            /* Interrupted by signal */
            return SUCCESS;
        }
        MB_LOG_ERROR("select() error: %s", strerror(errno));
        return ERROR_IO;
    }

    if (ret == 0) {
        /* Timeout - no activity */
        return SUCCESS;
    }

    /* Check for serial port activity */
    if (serial_fd >= 0 && FD_ISSET(serial_fd, &readfds)) {
        bridge_process_serial_data(ctx);
    }

    /* Check for telnet activity */
    if (telnet_is_connected(&ctx->telnet)) {
        int telnet_fd = telnet_get_fd(&ctx->telnet);
        if (telnet_fd >= 0 && FD_ISSET(telnet_fd, &readfds)) {
            bridge_process_telnet_data(ctx);
        }
    }

    return SUCCESS;
}

/**
 * Print bridge statistics
 */
void bridge_print_stats(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    MB_LOG_INFO("=== Bridge Statistics ===");
    MB_LOG_INFO("Serial -> Telnet: %llu bytes",
                (unsigned long long)ctx->bytes_serial_to_telnet);
    MB_LOG_INFO("Telnet -> Serial: %llu bytes",
                (unsigned long long)ctx->bytes_telnet_to_serial);

    if (ctx->connection_start_time > 0) {
        time_t duration = time(NULL) - ctx->connection_start_time;
        MB_LOG_INFO("Connection duration: %ld seconds", (long)duration);
    }

    MB_LOG_INFO("========================");
}
