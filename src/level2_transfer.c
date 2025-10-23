/*
 * level2_transfer.c - Level 2 (Telnet) Data Transfer Implementation
 *
 * This file implements data transfer functions between telnet and serial
 * interfaces, including protocol processing and ANSI sequence handling.
 */

#ifdef ENABLE_LEVEL2

#include "bridge.h"
#include "level2_transfer.h"
#include "level2_connection.h"
#include "level1_encoding.h"
#include "telnet.h"
#include "modem.h"
#include "serial.h"
#include "datalog.h"
#include "common.h"
#include <unistd.h>
#include <string.h>

/**
 * Process data from telnet (Level 2 only)
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
 * Transfer data from telnet to serial (Level 2 only)
 */
int bridge_transfer_telnet_to_serial(bridge_ctx_t *ctx)
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

    /* Log data from telnet (raw, before IAC processing) */
    datalog_write(&ctx->datalog, DATALOG_DIR_FROM_TELNET, telnet_buf, n);

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

    /* Log data to modem (after IAC processing, ready to send) */
    datalog_write(&ctx->datalog, DATALOG_DIR_TO_MODEM, output_buf, output_len);

    /* Send to serial */
    ssize_t sent = serial_write(&ctx->serial, output_buf, output_len);
    if (sent > 0) {
        ctx->bytes_telnet_to_serial += sent;
    }

    return SUCCESS;
}

#endif /* ENABLE_LEVEL2 */