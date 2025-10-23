/*
 * level2_thread.c - Level 2 (Telnet) Thread Implementation
 *
 * This file implements the telnet thread function that handles all
 * Level 2 telnet I/O operations in a separate thread.
 */

#ifdef ENABLE_LEVEL2

#include "bridge.h"
#include "level2_thread.h"
#include "level1_buffer.h"
#include "level1_encoding.h"
#include "telnet.h"
#include "modem.h"
#include "datalog.h"
#include "common.h"
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#ifdef ENABLE_LEVEL3
#include "level3.h"
#endif

/**
 * Telnet thread function
 * Handles telnet I/O, IAC processing, and Telnet<->Serial buffering
 * in multithread mode. Runs as a dedicated thread for Level 2 operations.
 */
void *telnet_thread_func(void *arg)
{
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;
    unsigned char telnet_buf[BUFFER_SIZE];
    unsigned char processed_buf[BUFFER_SIZE];
    unsigned char output_buf[BUFFER_SIZE];
    unsigned char tx_buf[BUFFER_SIZE];

    MB_LOG_INFO("[Thread 2] Telnet thread started");

    while (ctx->thread_running) {
        /* Check if telnet is connected or connecting */
        if (!telnet_is_connected(&ctx->telnet)) {
            /* Level 3 mode: Connection controlled by state machine, not by this thread */
            /* But we need to process events for non-blocking connect completion */
            if (ctx->telnet.is_connecting) {
                /* Process epoll events to complete non-blocking connection */
                int result = telnet_process_events(&ctx->telnet, 100);
                if (result != SUCCESS) {
                    MB_LOG_ERROR("[Thread 2] Failed to process connection events: %d", result);
                    telnet_disconnect(&ctx->telnet);
                }
                /* Check if connection completed */
                if (telnet_is_connected(&ctx->telnet)) {
                    MB_LOG_INFO("[Thread 2] Telnet connection completed");
                }
            }
            usleep(100000);  /* 100ms */
            continue;
        }

        /* === Part 1: Telnet → Serial direction === */

        /* Read from telnet */
        ssize_t n = telnet_recv(&ctx->telnet, telnet_buf, sizeof(telnet_buf));

        if (n < 0) {
            /* I/O error */
            MB_LOG_ERROR("[Thread 2] Telnet connection error");
            telnet_disconnect(&ctx->telnet);

            /* Notify serial thread to hang up modem */
            pthread_mutex_lock(&ctx->modem_mutex);
            if (modem_is_online(&ctx->modem)) {
                modem_hangup(&ctx->modem);
                modem_send_no_carrier(&ctx->modem);
            }
            pthread_mutex_unlock(&ctx->modem_mutex);

            pthread_mutex_lock(&ctx->state_mutex);
            ctx->state = STATE_IDLE;
            pthread_mutex_unlock(&ctx->state_mutex);

            continue;
        }

        if (n == 0) {
            /* Check if connection closed */
            if (!telnet_is_connected(&ctx->telnet)) {
                MB_LOG_INFO("[Thread 2] Telnet disconnected");
                telnet_disconnect(&ctx->telnet);

                pthread_mutex_lock(&ctx->modem_mutex);
                if (modem_is_online(&ctx->modem)) {
                    modem_hangup(&ctx->modem);
                    modem_send_no_carrier(&ctx->modem);
                }
                pthread_mutex_unlock(&ctx->modem_mutex);

                pthread_mutex_lock(&ctx->state_mutex);
                ctx->state = STATE_IDLE;
                pthread_mutex_unlock(&ctx->state_mutex);
            }
            usleep(1000);
            continue;
        }

        if (n > 0) {
            /* Log data from telnet */
            datalog_write(&ctx->datalog, DATALOG_DIR_FROM_TELNET, telnet_buf, n);

            /* Process telnet protocol (remove IAC) */
            size_t processed_len;
            telnet_process_input(&ctx->telnet, telnet_buf, n,
                                processed_buf, sizeof(processed_buf), &processed_len);

            if (processed_len > 0) {
                /* Pass through ANSI sequences to modem client */
                size_t output_len;
                ansi_passthrough_telnet_to_modem(processed_buf, processed_len,
                                                output_buf, sizeof(output_buf), &output_len);

                if (output_len > 0) {
                    /* Write to telnet→serial buffer */
                    ts_cbuf_write(&ctx->ts_telnet_to_serial_buf,
                                 output_buf, output_len);
                }
            }
        }

        /* === Part 2: Serial → Telnet direction === */

        /* Check if Level 3 is handling this direction */
        bool level3_handles_serial_to_telnet = false;
#ifdef ENABLE_LEVEL3
        if (ctx->level3_enabled) {
            /* If Level 3 is enabled at all, it handles the buffers */
            level3_handles_serial_to_telnet = true;

            /* DEBUG: Log Level 3 handling status periodically */
            static int log_count = 0;
            if (++log_count % 1000 == 0) {
                if (ctx->level3 != NULL) {
                    l3_context_t *l3_ctx = (l3_context_t*)ctx->level3;
                    printf("[DEBUG-TELNET-THREAD] Level 3 enabled: active=%d, state=%d\n",
                           l3_ctx->level3_active, l3_ctx->system_state);
                    fflush(stdout);
                }
            }
        }
#endif

        /* Level 2 mode: Read from serial→telnet buffer and send (ONLY if Level 3 not enabled) */
        if (!level3_handles_serial_to_telnet) {
            size_t tx_len = ts_cbuf_read(&ctx->ts_serial_to_telnet_buf, tx_buf, sizeof(tx_buf));
            if (tx_len > 0) {
                /* Log data to telnet */
                datalog_write(&ctx->datalog, DATALOG_DIR_TO_TELNET, tx_buf, tx_len);

                /* Send to telnet server */
                telnet_send(&ctx->telnet, tx_buf, tx_len);
            }
        }

        /* Short sleep to avoid busy-waiting */
        usleep(10000);  /* 10ms - reduced frequency to avoid excessive polling */
    }

    MB_LOG_INFO("[Thread 2] Telnet thread exiting");
    return NULL;
}

#endif /* ENABLE_LEVEL2 */