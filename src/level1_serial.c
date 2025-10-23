/*
 * level1_serial.c - Serial data processing functions for Level 1
 *
 * This file contains implementations for serial data processing functions
 * specific to Level 1 (Serial/Modem only) operation.
 */

#include "bridge.h"
#include "level1_serial.h"
#include "level1_types.h"
#include "serial.h"
#include "modem.h"
#include "datalog.h"
#include "timestamp.h"
#include "echo.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#ifdef ENABLE_LEVEL1

/**
 * Process data from serial port - Level 1 exclusive implementation
 */
int bridge_process_serial_data(bridge_ctx_t *ctx)
{
    unsigned char buf[BUFFER_SIZE];
    ssize_t n;

    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Read from serial port */
    struct timeval tv_before, tv_after;
    gettimeofday(&tv_before, NULL);

    printf("[DEBUG] [Level 1] bridge_process_serial_data: About to call serial_read()\n");
    fflush(stdout);
    n = serial_read(&ctx->serial, buf, sizeof(buf));

    gettimeofday(&tv_after, NULL);
    long elapsed_us = (tv_after.tv_sec - tv_before.tv_sec) * 1000000L +
                      (tv_after.tv_usec - tv_before.tv_usec);

    printf("[DEBUG] [Level 1] bridge_process_serial_data: serial_read() returned %zd (took %ld us)\n", n, elapsed_us);
    fflush(stdout);

    if (n > 0) {
        /* Log serial data for debugging */
        level1_log_serial_data(buf, n, "[Level 1] Serial RX");

        /* Log data from modem */
        datalog_write(&ctx->datalog, DATALOG_DIR_FROM_MODEM, buf, n);

        /* Check for hardware modem messages */
        bool hardware_msg_handled = level1_check_hardware_messages(ctx, (char *)buf, n);

        /* Check modem state */
        modem_state_t current_state = modem_get_state(&ctx->modem);

        if (current_state == MODEM_STATE_CONNECTING) {
            /* Modem is connecting - wait for hardware CONNECT */
            MB_LOG_INFO("[Level 1] Modem in CONNECTING state - waiting for hardware CONNECT response");
            return SUCCESS;
        } else if (current_state == MODEM_STATE_ONLINE) {
            /* Process online mode */
            return level1_process_online_mode(ctx, buf, n);
        } else if (current_state == MODEM_STATE_DISCONNECTED) {
            MB_LOG_INFO("[Level 1] Modem state changed to DISCONNECTED");
            ctx->state = STATE_IDLE;
            return SUCCESS;
        }

        /* Process command mode if not a hardware message */
        if (!hardware_msg_handled && !modem_is_online(&ctx->modem)) {
            return level1_process_command_mode(ctx, buf, n);
        }

        /* Check for escape sequence in online mode */
        ssize_t consumed = modem_process_input(&ctx->modem, (char *)buf, n);
        if (!modem_is_online(&ctx->modem)) {
            /* Modem went offline (escape sequence detected) */
            return SUCCESS;
        }

        if (consumed > 0) {
            /* Process echo if enabled */
            if (ctx->config->echo_enabled) {
                echo_result_t echo_result = echo_process_client_data(&ctx->echo, &ctx->serial,
                                                                      buf, consumed);
                switch (echo_result) {
                    case ECHO_SUCCESS:
                        MB_LOG_DEBUG("[Level 1] Echo processed successfully");
                        break;
                    case ECHO_DISABLED:
                        MB_LOG_DEBUG("[Level 1] Echo functionality disabled");
                        break;
                    case ECHO_ERROR:
                        MB_LOG_WARNING("[Level 1] Echo processing failed");
                        break;
                    case ECHO_INVALID_PARAM:
                        MB_LOG_WARNING("[Level 1] Echo processing: invalid parameters");
                        break;
                    case ECHO_BUFFER_FULL:
                        MB_LOG_WARNING("[Level 1] Echo buffer full");
                        break;
                }
            }
        }

        return SUCCESS;
    } else if (n == 0) {
        /* No data available */
        printf("[DEBUG] [Level 1] bridge_process_serial_data: serial_read() returned 0 (no data or EAGAIN)\n");
        fflush(stdout);
        MB_LOG_DEBUG("[Level 1] serial_read() returned 0 (no data available)");
        return SUCCESS;
    } else {
        /* Serial I/O error */
        return level1_handle_serial_error(ctx, errno);
    }
}

/**
 * Process serial data in command mode
 */
int level1_process_command_mode(bridge_ctx_t *ctx, unsigned char *buf, size_t len)
{
    if (ctx == NULL || buf == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Let modem process the AT commands */
    modem_process_input(&ctx->modem, (char *)buf, len);
    return SUCCESS;
}

/**
 * Process serial data in online mode
 */
int level1_process_online_mode(bridge_ctx_t *ctx, unsigned char *buf, size_t len)
{
    if (ctx == NULL || buf == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Note: buf is validated but not used directly, len is used in logging */
    (void)buf; /* Suppress unused warning - validated above */

#ifndef DEBUG
    /* In non-DEBUG builds, MB_LOG_DEBUG is compiled out, so len appears unused */
    (void)len; /* Suppress unused warning - used in MB_LOG_DEBUG below */
#endif

    MB_LOG_INFO("[Level 1] Hardware modem ONLINE - direct modem<->client communication");

    /* Mark client connection as established */
    if (!ctx->client_data_received) {
        ctx->client_data_received = true;
        printf("[INFO] [Level 1] Client connected (CONNECT received) - timestamp transmission enabled\n");
        fflush(stdout);
        MB_LOG_INFO("[Level 1] Client connected (CONNECT received) - timestamp transmission enabled");

        /* Start timestamp transmission */
        printf("[DEBUG] [Level 1] Calling timestamp_set_online() to start timestamp tracking\n");
        fflush(stdout);
        timestamp_set_online(&ctx->timestamp);
        MB_LOG_INFO("[Level 1] Timestamp tracking started - first timestamp in 3 seconds");

        /* Activate echo if configured */
        printf("[DEBUG] [Level 1] Checking echo configuration: echo_enabled=%d\n", ctx->config->echo_enabled);
        fflush(stdout);
        MB_LOG_DEBUG("[Level 1] Echo configuration: echo_enabled=%d", ctx->config->echo_enabled);

        if (ctx->config->echo_enabled) {
            printf("[DEBUG] [Level 1] Activating echo functionality for ONLINE client\n");
            fflush(stdout);
            echo_set_online(&ctx->echo);
            MB_LOG_INFO("[Level 1] Echo functionality activated - client is ONLINE");
        } else {
            printf("[DEBUG] [Level 1] Echo functionality disabled - not activating\n");
            fflush(stdout);
            MB_LOG_DEBUG("[Level 1] Echo functionality disabled - not activating");
        }
    }

    /* Update connection state */
    ctx->state = STATE_CONNECTED;
    ctx->connection_start_time = time(NULL);

    MB_LOG_DEBUG("[Level 1] Processed %zu bytes from client (no telnet forward)", len);

    return SUCCESS;
}

/**
 * Log serial data with hex dump
 */
void level1_log_serial_data(const unsigned char *buf, size_t len, const char *prefix)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("[INFO] [%s] %s: %zd bytes >>>\n", timestamp, prefix, len);
    fflush(stdout);
    MB_LOG_INFO("%s: %zd bytes >>>", prefix, len);

    /* Print hex dump (limit to first 64 bytes) */
    char hex_str[BUFFER_SIZE * 3 + 1];
    size_t display_len = (len < 64) ? len : 64;

    for (size_t i = 0; i < display_len; i++) {
        snprintf(hex_str + i*3, 4, "%02X ", (unsigned char)buf[i]);
    }

    printf("[INFO] [%s]   DATA: [%s]\n", timestamp, hex_str);
    fflush(stdout);
    MB_LOG_INFO("  DATA: [%s]", hex_str);
}

/**
 * Handle serial I/O error
 */
int level1_handle_serial_error(bridge_ctx_t *ctx, int error_code)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_ERROR("[Level 1] Serial I/O error detected: %s", strerror(error_code));

    /* Close serial port */
    serial_close(&ctx->serial);

    /* Transition to DISCONNECTED state */
    ctx->serial_ready = false;
    ctx->modem_ready = false;
    ctx->last_serial_retry = time(NULL);
    ctx->state = STATE_IDLE;

    MB_LOG_WARNING("[Level 1] Transitioned to DISCONNECTED state (will retry in %d seconds)",
                  ctx->serial_retry_interval);

    return ERROR_IO;
}

/**
 * Check and process hardware modem messages
 */
bool level1_check_hardware_messages(bridge_ctx_t *ctx, const char *buf, size_t len)
{
    if (ctx == NULL || buf == NULL) {
        return false;
    }

    /* Check for hardware modem unsolicited messages */
    return modem_process_hardware_message(&ctx->modem, buf, len);
}

/**
 * Handle modem connect event - Level 1 version
 */
int bridge_handle_modem_connect_level1(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("[Level 1] === Hardware modem CONNECT received - no telnet connection ===");

    /* Ensure modem is in online state */
    if (!modem_is_online(&ctx->modem)) {
        MB_LOG_INFO("[Level 1] Setting modem to online mode");
        modem_go_online(&ctx->modem);
    }

    /* Send CONNECT message if needed */
    modem_send_connect(&ctx->modem, ctx->config->baudrate_value);

    /* Update state */
    ctx->state = STATE_CONNECTED;
    ctx->connection_start_time = time(NULL);

    /* Initialize echo and timestamp */
    if (ctx->config->echo_enabled) {
        echo_set_online(&ctx->echo);
        MB_LOG_INFO("[Level 1] Echo functionality activated");
    }

    timestamp_set_online(&ctx->timestamp);
    MB_LOG_INFO("[Level 1] Timestamp functionality activated");

    MB_LOG_INFO("[Level 1] === Bridge connection ready (Level 1 mode) ===");

    return SUCCESS;
}

/**
 * Handle modem disconnect event - Level 1 version
 */
int bridge_handle_modem_disconnect_level1(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("[Level 1] === Modem disconnect event ===");

    /* Update modem state */
    if (modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
    }

    /* Disable echo and timestamp */
    if (ctx->config->echo_enabled) {
        echo_set_offline(&ctx->echo);
        MB_LOG_INFO("[Level 1] Echo functionality deactivated");
    }

    timestamp_set_offline(&ctx->timestamp);
    MB_LOG_INFO("[Level 1] Timestamp functionality deactivated");

    /* Update state */
    ctx->state = STATE_IDLE;
    ctx->client_data_received = false;

    /* Log connection duration if we were connected */
    if (ctx->connection_start_time > 0) {
        time_t duration = time(NULL) - ctx->connection_start_time;
        MB_LOG_INFO("[Level 1] Connection duration: %ld seconds", duration);
        ctx->connection_start_time = 0;
    }

    MB_LOG_INFO("[Level 1] === Disconnected ===");

    return SUCCESS;
}

/**
 * Initialize Level 1 serial processing
 */
int level1_serial_init(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("[Level 1] Initializing serial processing");

    /* Initialize timestamp with Level 1 format */
    timestamp_set_format(&ctx->timestamp, "[Level 1]", "Active", true, true);

    /* Initialize echo if enabled */
    if (ctx->config->echo_enabled) {
        MB_LOG_INFO("[Level 1] Echo functionality enabled in configuration");
    }

    /* Set serial retry parameters */
    ctx->serial_retry_interval = 10; /* Default 10 seconds */
    ctx->serial_retry_count = 0;

    MB_LOG_INFO("[Level 1] Serial processing initialized");

    return SUCCESS;
}

/**
 * Cleanup Level 1 serial processing
 */
void level1_serial_cleanup(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    MB_LOG_INFO("[Level 1] Cleaning up serial processing");

    /* Ensure we're disconnected */
    if (ctx->state == STATE_CONNECTED) {
        bridge_handle_modem_disconnect_level1(ctx);
    }

    /* Close serial port if open */
    if (ctx->serial_ready) {
        serial_close(&ctx->serial);
        ctx->serial_ready = false;
    }

    MB_LOG_INFO("[Level 1] Serial processing cleanup complete");
}

#endif /* ENABLE_LEVEL1 */