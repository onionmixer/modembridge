/*
 * level1_thread.c - Serial/Modem thread implementation for Level 1
 *
 * This file contains the Serial/Modem thread implementation that handles
 * all Level 1 operations including serial I/O, health checks, timestamp
 * transmission, and echo functionality.
 */

#include "bridge.h"
#include "level1_thread.h"
#include "serial.h"
#include "modem.h"
#include "timestamp.h"
#include "echo.h"
#include "datalog.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#ifdef ENABLE_LEVEL1

#ifdef ENABLE_LEVEL2
#include "telnet.h"
#endif

#ifdef ENABLE_LEVEL3
#include "level3.h"
#endif

/**
 * Serial/Modem thread main function
 */
void *serial_modem_thread_func(void *arg)
{
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;
    unsigned char serial_buf[BUFFER_SIZE];
#ifdef ENABLE_LEVEL2
    unsigned char tx_buf[BUFFER_SIZE];
#endif

    bool health_check_done = false;

    printf("[INFO] [Thread 1] Serial/Modem thread started\n");
    fflush(stdout);
    MB_LOG_INFO("[Thread 1] Serial/Modem thread started");

    while (ctx->thread_running) {
        /* Goal 1: Serial health check (once at startup) */
        if (!health_check_done && ctx->serial_ready) {
            printf("[INFO] [Thread 1] === Performing serial health check ===\n");
            fflush(stdout);
            MB_LOG_INFO("[Thread 1] === Performing serial health check ===");

            /* Verify serial port is readable/writable */
            int serial_fd = serial_get_fd(&ctx->serial);
            if (serial_fd >= 0) {
                MB_LOG_INFO("[Thread 1] Serial port health check: OK (fd=%d)", serial_fd);
            } else {
                MB_LOG_WARNING("[Thread 1] Serial port health check: FAILED (invalid fd)");
            }

            /* Goal 2: Verify modem initialization */
            if (ctx->modem_ready) {
                MB_LOG_INFO("[Thread 1] Modem initialization: OK");
                MB_LOG_INFO("[Thread 1] Modem echo=%d, verbose=%d, quiet=%d",
                           ctx->modem.settings.echo,
                           ctx->modem.settings.verbose,
                           ctx->modem.settings.quiet);
            } else {
                MB_LOG_WARNING("[Thread 1] Modem initialization: NOT READY");
            }

            MB_LOG_INFO("[Thread 1] === Health check completed ===");
            health_check_done = true;

            /* IMPORTANT: Check if modem is already ONLINE after draining phase */
            /* This can happen if CONNECT was received during initialization */
            pthread_mutex_lock(&ctx->modem_mutex);
            bool already_online = modem_is_online(&ctx->modem);
            pthread_mutex_unlock(&ctx->modem_mutex);

            if (already_online) {
                printf("[INFO] [Thread 1] Modem is ALREADY ONLINE (CONNECT received during init)\n");
                printf("[INFO] [Thread 1] Enabling timestamp transmission\n");
                fflush(stdout);
                MB_LOG_INFO("[Thread 1] Modem is ALREADY ONLINE (CONNECT received during init)");
                MB_LOG_INFO("[Thread 1] Enabling timestamp transmission");

                /* Start timestamp transmission */
                timestamp_set_online(&ctx->timestamp);

                /* Set flag to enable timestamp transmission */
                ctx->client_data_received = true;

                /* Update connection state */
                pthread_mutex_lock(&ctx->state_mutex);
                ctx->state = STATE_CONNECTED;
                ctx->connection_start_time = time(NULL);
                pthread_mutex_unlock(&ctx->state_mutex);
            }
        }

        /* Check if serial port is ready */
        if (!ctx->serial_ready) {
            usleep(100000);  /* 100ms */
            continue;
        }

        /* Goal 4: Send timestamp using modular timestamp system when modem is ONLINE */
        pthread_mutex_lock(&ctx->modem_mutex);
        bool is_online = modem_is_online(&ctx->modem);
        pthread_mutex_unlock(&ctx->modem_mutex);

        if (is_online && ctx->client_data_received) {
            /* Check if timestamp should be sent using modular system */
            if (timestamp_should_send(&ctx->timestamp)) {
                printf("[INFO] [Thread 1] Sending timestamp via modular system\n");
                fflush(stdout);
                MB_LOG_INFO("[Thread 1] Sending timestamp via modular system");

                /* Send timestamp using modular function (epoll-based write) */
                timestamp_result_t result = timestamp_send(&ctx->serial, &ctx->timestamp);

                switch (result) {
                    case TIMESTAMP_SUCCESS:
                        printf("[INFO] [Thread 1] Timestamp sent successfully via modular system\n");
                        fflush(stdout);
                        break;

                    case TIMESTAMP_TIMEOUT:
                        printf("[WARNING] [Thread 1] Timestamp send timeout\n");
                        fflush(stdout);
                        MB_LOG_WARNING("[Thread 1] Timestamp send timeout");
                        break;

                    case TIMESTAMP_ERROR:
                        printf("[ERROR] [Thread 1] Timestamp send failed - forcing modem OFFLINE\n");
                        fflush(stdout);
                        MB_LOG_ERROR("[Thread 1] Timestamp send failed - forcing modem OFFLINE");

                        /* Force modem to OFFLINE state on write error */
                        pthread_mutex_lock(&ctx->modem_mutex);
                        modem_hangup(&ctx->modem);
                        pthread_mutex_unlock(&ctx->modem_mutex);

                        /* Reset timestamp state */
                        timestamp_set_offline(&ctx->timestamp);
                        ctx->client_data_received = false;

                        /* === ECHO FUNCTIONALITY: Mark client as OFFLINE === */
                        echo_set_offline(&ctx->echo);
                        MB_LOG_INFO("[Thread 1] Echo functionality deactivated - client is OFFLINE");
                        break;

                    case TIMESTAMP_DISABLED:
                        /* Should not happen if enabled, but handle gracefully */
                        printf("[DEBUG] [Thread 1] Timestamp transmission disabled\n");
                        fflush(stdout);
                        break;

                    case TIMESTAMP_NOT_DUE:
                        /* Should not happen since we checked timestamp_should_send */
                        break;
                }
            }
        } else {
            /* Reset timestamp state when modem goes offline */
            static bool was_online = false;
            if (!is_online && was_online) {
                printf("[INFO] [Thread 1] Modem went OFFLINE, resetting timestamp state\n");
                fflush(stdout);
                MB_LOG_INFO("[Thread 1] Modem went OFFLINE, resetting timestamp state");
                timestamp_set_offline(&ctx->timestamp);
                ctx->client_data_received = false;  /* Reset for next connection */

                /* === ECHO FUNCTIONALITY: Mark client as OFFLINE === */
                echo_set_offline(&ctx->echo);
                MB_LOG_INFO("[Thread 1] Echo functionality deactivated - client is OFFLINE");
            }
            was_online = is_online;
        }

        /* Goal 3: Wait for modem signals (AT commands, etc.) */

        /* === Part 1: Serial → Telnet direction === */

        /* Read from serial port */
        ssize_t n = serial_read(&ctx->serial, serial_buf, sizeof(serial_buf));

        if (n < 0) {
            /* I/O error - handle disconnection */
            MB_LOG_ERROR("[Thread 1] Serial I/O error: %s", strerror(errno));
            serial_close(&ctx->serial);
            ctx->serial_ready = false;
            ctx->modem_ready = false;
            usleep(100000);
            continue;
        }

        if (n > 0) {
            /* Log raw data for debugging */
            printf("[DEBUG] [Thread 1] Raw serial data (%zd bytes): ", n);
            for (ssize_t i = 0; i < n && i < 32; i++) {
                printf("%02X ", (unsigned char)serial_buf[i]);
            }
            printf("\n");
            printf("[DEBUG] [Thread 1] ASCII: [%.*s]\n", (int)n, serial_buf);
            fflush(stdout);

            /* Log data */
            datalog_write(&ctx->datalog, DATALOG_DIR_FROM_MODEM, serial_buf, n);

            /* === CRITICAL: Check for hardware modem messages first === */
            /* Hardware modems send unsolicited messages like RING, CONNECT, NO CARRIER */
            pthread_mutex_lock(&ctx->modem_mutex);
            bool hardware_msg_handled = modem_process_hardware_message(&ctx->modem, (char *)serial_buf, n);
            modem_state_t current_state = modem_get_state(&ctx->modem);

            /* === ADDITIONAL RING DETECTION FOR STDOUT === */
            /* Check if RING was in the received data for explicit stdout logging */
            if (strstr((char *)serial_buf, "RING") != NULL) {
                printf("[INFO] [Thread 1] *** RING SIGNAL DETECTED FROM HARDWARE MODEM ***\n");
                fflush(stdout);
                MB_LOG_INFO("[Thread 1] *** RING SIGNAL DETECTED FROM HARDWARE MODEM ***");

                /* Get ring count for additional logging */
                int ring_count = ctx->modem.settings.s_registers[SREG_RING_COUNT];
                int auto_answer = ctx->modem.settings.s_registers[SREG_AUTO_ANSWER];
                printf("[INFO] [Thread 1] Ring count: %d, Auto-answer setting (S0): %d\n",
                       ring_count, auto_answer);
                fflush(stdout);
            }

            pthread_mutex_unlock(&ctx->modem_mutex);

            /* Handle state transitions from hardware messages */
            if (current_state == MODEM_STATE_CONNECTING) {
                /* Modem is connecting (answered call, waiting for CONNECT) */
                /* Check if this is software or hardware auto-answer mode */
                int auto_answer = ctx->modem.settings.s_registers[SREG_AUTO_ANSWER];
                if (auto_answer == 0) {
                    /* Software auto-answer mode (S0=0) */
                    printf("[INFO] [Thread 1] Modem state: CONNECTING - waiting for CONNECT response (software auto-answer mode)\n");
                    fflush(stdout);
                    MB_LOG_INFO("[Thread 1] Modem state: CONNECTING - waiting for CONNECT response (software auto-answer mode)");
                } else {
                    /* Hardware auto-answer mode (S0 > 0) */
                    printf("[INFO] [Thread 1] Modem state: CONNECTING - waiting for CONNECT response (hardware auto-answer mode, S0=%d)\n", auto_answer);
                    fflush(stdout);
                    MB_LOG_INFO("[Thread 1] Modem state: CONNECTING - waiting for CONNECT response (hardware auto-answer mode, S0=%d)", auto_answer);
                }
                continue;
            } else if (current_state == MODEM_STATE_ONLINE && hardware_msg_handled) {
                /* === ONLINE state transition: CONNECT message just received === */
                /* This block runs ONCE when CONNECT is received, not for every data byte */
                printf("[INFO] [Thread 1] Hardware modem ONLINE (CONNECT received)\n");
                fflush(stdout);
                MB_LOG_INFO("[Thread 1] Hardware modem ONLINE (CONNECT received)");

                /* Mark that client connection is established (ONLY after CONNECT) */
                if (!ctx->client_data_received) {
                    ctx->client_data_received = true;
                    printf("[INFO] [Thread 1] Client connected (CONNECT received) - timestamp transmission enabled\n");
                    fflush(stdout);
                    MB_LOG_INFO("[Thread 1] Client connected (CONNECT received) - timestamp transmission enabled");

                    /* IMPORTANT: Start timestamp transmission when modem goes ONLINE */
                    printf("[DEBUG] [Thread 1] Calling timestamp_set_online() to start timestamp tracking\n");
                    fflush(stdout);
                    timestamp_set_online(&ctx->timestamp);
                    MB_LOG_INFO("[Thread 1] Timestamp tracking started - first timestamp in 3 seconds");

                    /* === ECHO FUNCTIONALITY: Mark client as ONLINE for echo === */
                    printf("[DEBUG] [Thread 1] Checking echo configuration: echo_enabled=%d\n", ctx->config->echo_enabled);
                    fflush(stdout);
                    MB_LOG_DEBUG("[Thread 1] Echo configuration: echo_enabled=%d", ctx->config->echo_enabled);

                    if (ctx->config->echo_enabled) {
                        printf("[DEBUG] [Thread 1] Activating echo functionality for ONLINE client\n");
                        fflush(stdout);
                        echo_set_online(&ctx->echo);
                        MB_LOG_INFO("[Thread 1] Echo functionality activated - client is ONLINE");
                    } else {
                        printf("[DEBUG] [Thread 1] Echo functionality disabled - not activating\n");
                        fflush(stdout);
                        MB_LOG_DEBUG("[Thread 1] Echo functionality disabled - not activating");
                    }
                }

                /* Update state to indicate connection is active */
                pthread_mutex_lock(&ctx->state_mutex);
                ctx->state = STATE_CONNECTED;
                ctx->connection_start_time = time(NULL);
                pthread_mutex_unlock(&ctx->state_mutex);

                /* Note: This block runs only once when CONNECT message is received */
                /* Normal data processing happens in the else block below (line 2304+) */
                continue;
            } else if (current_state == MODEM_STATE_DISCONNECTED) {
                printf("[INFO] [Thread 1] Modem DISCONNECTED\n");
                fflush(stdout);
                MB_LOG_INFO("[Thread 1] Modem DISCONNECTED");
                pthread_mutex_lock(&ctx->state_mutex);
#ifdef ENABLE_LEVEL2
                if (telnet_is_connected(&ctx->telnet)) {
                    MB_LOG_INFO("[Thread 1] Closing telnet due to modem disconnect");
                    telnet_disconnect(&ctx->telnet);
                }
#endif
                ctx->state = STATE_IDLE;
                pthread_mutex_unlock(&ctx->state_mutex);
                continue;
            }

            /* Process through modem layer */
            pthread_mutex_lock(&ctx->modem_mutex);
            bool modem_online = modem_is_online(&ctx->modem);

            if (!modem_online) {
                /* COMMAND mode: process AT commands ONLY if not a hardware message */
                if (!hardware_msg_handled) {
                    modem_process_input(&ctx->modem, (char *)serial_buf, n);
                }
                pthread_mutex_unlock(&ctx->modem_mutex);
            } else {
                /* ONLINE mode: Process escape sequences */
                ssize_t consumed = modem_process_input(&ctx->modem, (char *)serial_buf, n);
                pthread_mutex_unlock(&ctx->modem_mutex);

                if (consumed > 0) {
#ifdef ENABLE_LEVEL3
                    /* === LEVEL 3 MODE: Forward data to pipeline === */
                    if (ctx->level3_enabled && ctx->level3 != NULL) {
                        l3_context_t *l3_ctx = (l3_context_t*)ctx->level3;

                        /* Debug: Print Level 3 status */
                        printf("[DEBUG-L3-STATUS] level3_active=%d, system_state=%d (DATA_TRANSFER=%d)\n",
                               l3_ctx->level3_active, l3_ctx->system_state, L3_STATE_DATA_TRANSFER);
                        fflush(stdout);

                        if (l3_ctx->level3_active && l3_ctx->system_state == L3_STATE_DATA_TRANSFER) {
                            /* Level 3 active: Write data to pipeline buffer */
                            size_t written = ts_cbuf_write(&ctx->ts_serial_to_telnet_buf,
                                                          serial_buf, consumed);
                            if (written > 0) {
                                printf("[DEBUG-L3-FORWARDED] Forwarded %zu bytes to pipeline\n", written);
                                fflush(stdout);
                                MB_LOG_DEBUG("[Thread 1] Level 3: Forwarded %zu bytes to pipeline", written);
                            } else {
                                printf("[WARNING-L3-BUFFER-FULL] Buffer full, dropped %zd bytes\n", consumed);
                                fflush(stdout);
                                MB_LOG_WARNING("[Thread 1] Level 3: Buffer full, dropped %zd bytes", consumed);
                            }
                            continue;  /* Skip Level 1 echo processing */
                        } else {
                            /* Debug: Why Level 3 not active? */
                            printf("[DEBUG-L3-INACTIVE] Skipping Level 3: active=%d, state=%d\n",
                                   l3_ctx->level3_active, l3_ctx->system_state);
                            fflush(stdout);
                        }
                    } else {
                        /* Debug: Level 3 not enabled or NULL */
                        printf("[DEBUG-L3-DISABLED] level3_enabled=%d, level3=%p\n",
                               ctx->level3_enabled, (void*)ctx->level3);
                        fflush(stdout);
                    }
#endif

#ifdef ENABLE_LEVEL2
                    /* === LEVEL 2 MODE: Forward data to telnet thread via buffer === */
                    size_t written = ts_cbuf_write(&ctx->ts_serial_to_telnet_buf,
                                                   serial_buf, consumed);
                    if (written > 0) {
                        MB_LOG_DEBUG("[Thread 1] Level 2: Forwarded %zu bytes to telnet thread", written);
                    } else {
                        MB_LOG_WARNING("[Thread 1] Level 2: Buffer full, dropped %zd bytes", consumed);
                    }
                    continue;  /* Skip Level 1 echo processing */
#endif

                    /* === LEVEL 1: Data is handled directly between modem and client === */
                    /* No telnet forwarding needed - client data stays local */
                    MB_LOG_DEBUG("[Thread 1] Level 1: Processed %zd bytes from client (no telnet forward)", consumed);

                    /* === ECHO FUNCTIONALITY (Level 1) === */
                    /* Process client data for echo with timestamp formatting */
                    if (ctx->config->echo_enabled) {
                        echo_result_t echo_result = echo_process_client_data(&ctx->echo, &ctx->serial,
                                                                          serial_buf, consumed);
                        switch (echo_result) {
                            case ECHO_SUCCESS:
                                MB_LOG_DEBUG("[Thread 1] Echo processed successfully");
                                break;
                            case ECHO_DISABLED:
                                MB_LOG_DEBUG("[Thread 1] Echo functionality disabled");
                                break;
                            case ECHO_ERROR:
                                MB_LOG_WARNING("[Thread 1] Echo processing failed");
                                break;
                            case ECHO_INVALID_PARAM:
                                MB_LOG_WARNING("[Thread 1] Echo processing: invalid parameters");
                                break;
                            case ECHO_BUFFER_FULL:
                                MB_LOG_WARNING("[Thread 1] Echo buffer full");
                                break;
                        }
                    }
                }
            }
        }

        /* === Part 2: Telnet → Serial direction === */
#ifdef ENABLE_LEVEL2

        /* Check if Level 3 is handling this direction */
        bool level3_handles_telnet_to_serial = false;
#ifdef ENABLE_LEVEL3
        if (ctx->level3_enabled && ctx->level3 != NULL) {
            l3_context_t *l3_ctx = (l3_context_t*)ctx->level3;
            if (l3_ctx->level3_active && l3_ctx->system_state == L3_STATE_DATA_TRANSFER) {
                /* Level 3 pipeline is handling data transfer - skip buffer reading */
                level3_handles_telnet_to_serial = true;
            }
        }
#endif

        /* Level 2 mode: Read from telnet→serial buffer and write to serial (if Level 3 not active) */
        if (!level3_handles_telnet_to_serial) {
            size_t tx_len = ts_cbuf_read(&ctx->ts_telnet_to_serial_buf, tx_buf, sizeof(tx_buf));
            if (tx_len > 0) {
                /* Log data */
                datalog_write(&ctx->datalog, DATALOG_DIR_TO_MODEM, tx_buf, tx_len);

                /* Write to serial port */
                ssize_t sent = serial_write(&ctx->serial, tx_buf, tx_len);
                if (sent > 0) {
                    ctx->bytes_telnet_to_serial += sent;
                }
            }
        }
#else
        /* Level 1: No telnet to serial transfer needed */
#endif

        /* Sleep longer to reduce CPU usage and prevent timestamp flooding */
        /* Check every 100ms instead of 10ms - still responsive but less busy */
        usleep(100000);  /* 100ms - balanced between responsiveness and CPU usage */
    }

    MB_LOG_INFO("[Thread 1] Serial/Modem thread exiting");
    return NULL;
}

#endif /* ENABLE_LEVEL1 */