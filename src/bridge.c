/*
 * bridge.c - Main bridging logic for ModemBridge
 */

#include "bridge.h"
#include "level1_buffer.h"   /* Level 1 buffer management functions */
#include "level1_encoding.h" /* Level 1 character encoding functions */
#ifdef ENABLE_LEVEL1
#include "level1_serial.h"   /* Level 1 serial processing functions */
#endif
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef ENABLE_LEVEL3
#include "level3.h"
#endif

/* Forward declarations */
#ifdef ENABLE_LEVEL2
static int bridge_transfer_telnet_to_serial(bridge_ctx_t *ctx);
static void bridge_sync_echo_mode(bridge_ctx_t *ctx);
#endif

/* ========== Buffer Management Functions (moved to level1_buffer.c) ========== */
/*
 * The following buffer management functions have been moved to level1_buffer.c:
 * - cbuf_init(), cbuf_write(), cbuf_read()
 * - cbuf_available(), cbuf_free(), cbuf_is_empty()
 * - cbuf_is_full(), cbuf_clear()
 * - ts_cbuf_init(), ts_cbuf_destroy()
 * - ts_cbuf_write(), ts_cbuf_read()
 * - ts_cbuf_write_timeout(), ts_cbuf_read_timeout()
 * - ts_cbuf_is_empty(), ts_cbuf_available()
 */

/* ========== Character Encoding Functions (moved to level1_encoding.c) ========== */
/*
 * The following character encoding functions have been moved to level1_encoding.c:
 * - is_utf8_start(), is_utf8_continuation()
 * - utf8_sequence_length(), is_valid_utf8_sequence()
 * - ansi_filter_modem_to_telnet()
 * - ansi_passthrough_telnet_to_modem()
 */

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

    /* Initialize connection state (modem_sample integration) */
    ctx->connected_baudrate = 0;
    ctx->carrier_detected = false;
    ctx->ring_count = 0;

    /* Initialize components */
    serial_init(&ctx->serial);
#ifdef ENABLE_LEVEL2
    telnet_init(&ctx->telnet);

    /* Link telnet to datalog for internal protocol logging */
    ctx->telnet.datalog = &ctx->datalog;
#endif

    /* Initialize buffers (legacy single-thread) */
#ifdef ENABLE_LEVEL2
    cbuf_init(&ctx->serial_to_telnet_buf);
    cbuf_init(&ctx->telnet_to_serial_buf);
#else
    /* Level 1: Telnet buffers not needed */
#endif

    /* Initialize thread-safe buffers (multithread mode) */
#ifdef ENABLE_LEVEL2
    ts_cbuf_init(&ctx->ts_serial_to_telnet_buf);
    ts_cbuf_init(&ctx->ts_telnet_to_serial_buf);
#else
    /* Level 1: Telnet buffers not needed */
#endif

    /* Initialize mutexes for shared state */
    pthread_mutex_init(&ctx->state_mutex, NULL);
    pthread_mutex_init(&ctx->modem_mutex, NULL);

    /* Thread control */
    ctx->thread_running = false;

    /* Initialize ANSI filter state */
    ctx->ansi_filter_state = ANSI_STATE_NORMAL;

    /* Initialize statistics */
#ifdef ENABLE_LEVEL2
    ctx->bytes_serial_to_telnet = 0;
    ctx->bytes_telnet_to_serial = 0;
#else
    /* Level 1: Telnet statistics not needed */
#endif
    ctx->connection_start_time = 0;

    /* Timestamp functionality (Level 1 only) */
#ifndef ENABLE_LEVEL2
    timestamp_init(&ctx->timestamp);
    timestamp_enable(&ctx->timestamp, 3, 10);  /* 3 sec first delay, 10 sec interval */

    /* Configure timestamp message format */
    timestamp_set_format(&ctx->timestamp, "[Level 1]", "Active", true, true);

    /* Configure transmission settings */
    timestamp_set_transmission(&ctx->timestamp, 1000, 3, 100);  /* 1s timeout, 3 retries, 100ms delay */

    /* Initialize echo functionality (Level 1 only) */
    echo_init(&ctx->echo);
    MB_LOG_INFO("Echo and timestamp subsystems initialized for Level 1");
#else
    MB_LOG_DEBUG("Echo/timestamp subsystems disabled for Level 2/3");
#endif

    /* Initialize data logger */
    datalog_init(&ctx->datalog);

#ifdef ENABLE_LEVEL3
    /* Initialize Level 3 system */
    ctx->level3_enabled = false;
    ctx->level3 = NULL;  /* Initialize as NULL, will be allocated during start */
    MB_LOG_DEBUG("Level 3 support compiled in, will initialize during start");
#endif

    MB_LOG_DEBUG("Bridge context initialized (thread-safe buffers and mutexes ready)");
}

/**
 * Start bridge operation (non-blocking)
 * Returns SUCCESS even if serial port is not available
 */
int bridge_start(bridge_ctx_t *ctx)
{
    if (ctx == NULL || ctx->config == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Starting bridge");

    /* Initialize retry state */
    ctx->serial_ready = false;
    ctx->modem_ready = false;
    ctx->last_serial_retry = 0;
    ctx->serial_retry_interval = 10;  /* 10 seconds */
    ctx->serial_retry_count = 0;

    /* Lock serial port using UUCP-style lock file (modem_sample approach) */
    printf("[DEBUG] Attempting to lock serial port: %s\n", ctx->config->serial_port);
    fflush(stdout);
    int lock_ret = serial_lock_port(ctx->config->serial_port);
    if (lock_ret != SUCCESS) {
        printf("[WARNING] Failed to lock serial port (already in use?), continuing anyway\n");
        fflush(stdout);
        MB_LOG_WARNING("Failed to lock serial port %s (may already be in use)",
                      ctx->config->serial_port);
        /* Continue anyway - some systems don't support UUCP locking */
    } else {
        printf("[DEBUG] Serial port locked successfully\n");
        fflush(stdout);
        MB_LOG_INFO("Serial port locked: %s", ctx->config->serial_port);
    }

    /* Try to open serial port (non-blocking) */
    printf("[DEBUG] Attempting to open serial port: %s\n", ctx->config->serial_port);
    fflush(stdout);
    int ret = serial_open(&ctx->serial, ctx->config->serial_port, ctx->config);
    printf("[DEBUG] serial_open() returned: %d (SUCCESS=%d)\n", ret, SUCCESS);
    fflush(stdout);

    if (ret == SUCCESS) {
        printf("[DEBUG] Serial port opened successfully, will flush buffers and send MODEM_AUTOANSWER_COMMAND\n");
        fflush(stdout);
        ctx->serial_ready = true;

        /* Flush any stale data in serial buffers */
        printf("[INFO] Flushing serial port buffers to clear stale data\n");
        fflush(stdout);
        serial_flush(&ctx->serial, TCIOFLUSH);
        MB_LOG_INFO("Serial port buffers flushed");

        /* Initialize modem */
        modem_init(&ctx->modem, &ctx->serial);

  #ifdef ENABLE_LEVEL3
        /* Register DCD event callback for Level 3 integration */
        /* This callback works in all levels (1, 2, 3) for DCD monitoring */
        printf("[DEBUG] Registering DCD event callback with modem\n");
        fflush(stdout);
        int dcd_ret = modem_set_dcd_event_callback(&ctx->modem,
                                                  (int (*)(void *, bool))bridge_handle_dcd_event,
                                                  ctx);
        if (dcd_ret == SUCCESS) {
            printf("[DEBUG] DCD event callback registered successfully\n");
            fflush(stdout);
            MB_LOG_DEBUG("DCD event callback registered with modem");
        } else {
            printf("[WARNING] Failed to register DCD event callback: %d\n", dcd_ret);
            fflush(stdout);
            MB_LOG_WARNING("Failed to register DCD event callback: %d", dcd_ret);
        }
#else
        /* Level 3 not available - DCD callback registration skipped */
        printf("[DEBUG] Level 3 not enabled, DCD event callback registration skipped\n");
        fflush(stdout);
        MB_LOG_DEBUG("Level 3 not enabled, DCD event callback registration skipped");
#endif

        ctx->modem_ready = true;

        /* === Execute MODEM_INIT_COMMAND first (hardware initialization) === */
        if (ctx->config->modem_init_command[0] != '\0') {
            char cmd_buf[LINE_BUFFER_SIZE * 2];
            unsigned char response[SMALL_BUFFER_SIZE];

            printf("[INFO] === Sending MODEM_INIT_COMMAND (hardware initialization) ===\n");
            fflush(stdout);
            MB_LOG_INFO("=== Sending MODEM_INIT_COMMAND (hardware initialization) ===");
            printf("[INFO] Command: %s\n", ctx->config->modem_init_command);
            fflush(stdout);
            MB_LOG_INFO("Command: %s", ctx->config->modem_init_command);

            /* Process commands separated by semicolons */
            char *cmd_copy = strdup(ctx->config->modem_init_command);
            char *token = strtok(cmd_copy, ";");

            while (token != NULL) {
                /* Skip leading/trailing spaces */
                while (*token == ' ') token++;
                char *end = token + strlen(token) - 1;
                while (end > token && *end == ' ') *end-- = '\0';

                if (*token != '\0') {
                    /* Build command with AT prefix if needed and \r\n suffix */
                    if (strncasecmp(token, "AT", 2) == 0) {
                        snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", token);
                    } else {
                        snprintf(cmd_buf, sizeof(cmd_buf), "AT%s\r\n", token);
                    }

                    /* Send command to hardware modem */
                    printf("[DEBUG] Sending: [%s]\n", cmd_buf);
                    fflush(stdout);
                    ssize_t sent = serial_write(&ctx->serial,
                                (const unsigned char *)cmd_buf,
                                strlen(cmd_buf));
                    printf("[INFO] Sent %zd bytes to hardware modem\n", sent);
                    fflush(stdout);

                    /* Wait for response */
                    usleep(200000);  /* 200ms between commands */
                    ssize_t resp_len = serial_read(&ctx->serial, response, sizeof(response));

                    if (resp_len > 0) {
                        printf("[INFO] Response (%zd bytes): [%.*s]\n",
                               resp_len, (int)resp_len, response);
                        fflush(stdout);
                        MB_LOG_INFO("Response: [%.*s]", (int)resp_len, response);
                    }

                    /* Update software modem state WITHOUT sending responses
                     * (responses already sent by hardware modem) */
                    /* DISABLED: modem_process_command() sends responses via serial_write()
                     * which causes infinite loop of OK/ERROR messages
                     * TODO: Implement state-only update without responses */
                }

                token = strtok(NULL, ";");
            }

            free(cmd_copy);

            printf("[INFO] === MODEM_INIT_COMMAND completed ===\n");
            fflush(stdout);
            MB_LOG_INFO("=== MODEM_INIT_COMMAND completed ===");

            /* Small delay before auto-answer command */
            usleep(500000);  /* 500ms */
        } else {
            printf("[DEBUG] MODEM_INIT_COMMAND is empty, skipping initialization\n");
            fflush(stdout);
        }

        /* Send auto-answer command to hardware modem (modem_sample MODE pattern) */
        /* Select command based on MODEM_AUTOANSWER_MODE (0=SOFTWARE, 1=HARDWARE) */
        const char *autoanswer_cmd = NULL;
        const char *mode_name = NULL;

        if (ctx->config->modem_autoanswer_mode == 0) {
            /* SOFTWARE mode: S0=0, manual ATA required */
            autoanswer_cmd = ctx->config->modem_autoanswer_software_command;
            mode_name = "SOFTWARE";
        } else {
            /* HARDWARE mode: S0>0, automatic answer */
            autoanswer_cmd = ctx->config->modem_autoanswer_hardware_command;
            mode_name = "HARDWARE";
        }

        printf("[DEBUG] MODEM_AUTOANSWER_MODE=%d (%s), command=[%s] (len=%zu)\n",
               ctx->config->modem_autoanswer_mode, mode_name,
               autoanswer_cmd, strlen(autoanswer_cmd));
        fflush(stdout);
        MB_LOG_INFO("MODEM_AUTOANSWER_MODE=%d (%s), command=[%s]",
                   ctx->config->modem_autoanswer_mode, mode_name, autoanswer_cmd);

        if (autoanswer_cmd[0] != '\0') {
            char cmd_buf[LINE_BUFFER_SIZE * 2];  /* Extra space for AT prefix and \r\n */
            unsigned char response[SMALL_BUFFER_SIZE];
            size_t cmd_len;

            printf("[INFO] === Sending MODEM_AUTOANSWER command (%s mode) ===\n", mode_name);
            fflush(stdout);
            MB_LOG_INFO("=== Sending MODEM_AUTOANSWER command (%s mode) ===", mode_name);
            printf("[INFO] Command: %s\n", autoanswer_cmd);
            fflush(stdout);
            MB_LOG_INFO("Command: %s", autoanswer_cmd);

            /* Build command with AT prefix and \r\n suffix */
            if (strncasecmp(autoanswer_cmd, "AT", 2) == 0) {
                /* Command already has AT prefix */
                cmd_len = strlen(autoanswer_cmd);
                if (cmd_len + 3 < sizeof(cmd_buf)) {  /* +3 for \r\n\0 */
                    SAFE_STRNCPY(cmd_buf, autoanswer_cmd, sizeof(cmd_buf));
                    strcat(cmd_buf, "\r\n");
                }
            } else {
                /* Prepend AT */
                cmd_len = strlen(autoanswer_cmd);
                if (cmd_len + 5 < sizeof(cmd_buf)) {  /* +5 for AT\r\n\0 */
                    strcpy(cmd_buf, "AT");
                    strcat(cmd_buf, autoanswer_cmd);
                    strcat(cmd_buf, "\r\n");
                }
            }

            /* Send command to hardware modem */
            printf("[DEBUG] About to call serial_write() with cmd_buf\n");
            fflush(stdout);
            ssize_t sent = serial_write(&ctx->serial,
                        (const unsigned char *)cmd_buf,
                        strlen(cmd_buf));
            printf("[INFO] Sent %zd bytes to hardware modem\n", sent);
            fflush(stdout);
            MB_LOG_INFO("Sent %zd bytes to hardware modem", sent);

            /* Wait for response (simple approach - just wait and read) */
            printf("[DEBUG] Waiting 500ms for modem response...\n");
            fflush(stdout);
            usleep(500000);  /* 500ms */
            ssize_t resp_len = serial_read(&ctx->serial, response, sizeof(response));
            printf("[DEBUG] serial_read() returned %zd bytes\n", resp_len);
            fflush(stdout);

            if (resp_len > 0) {
                printf("[INFO] Modem response (%zd bytes): [%.*s]\n", resp_len, (int)resp_len, response);
                fflush(stdout);
                MB_LOG_INFO("Modem response (%zd bytes): [%.*s]", resp_len, (int)resp_len, response);

                /* Update software modem state WITHOUT sending responses
                 * Parse S0 value from command manually to avoid modem_process_command()
                 * which would send OK via serial_write() causing infinite loop */
                const char *s0_pos = strstr(autoanswer_cmd, "S0=");
                if (s0_pos) {
                    int s0_value = atoi(s0_pos + 3);
                    ctx->modem.settings.s_registers[SREG_AUTO_ANSWER] = s0_value;
                    printf("[INFO] Software modem S0 register set to %d (%s mode)\n", s0_value, mode_name);
                    fflush(stdout);
                    MB_LOG_INFO("Software modem S0 register set to %d (%s mode)", s0_value, mode_name);
                } else {
                    printf("[WARNING] S0= not found in autoanswer command\n");
                    fflush(stdout);
                    MB_LOG_WARNING("S0= not found in autoanswer command");
                }
            } else {
                printf("[WARNING] No response from modem (or read error)\n");
                fflush(stdout);
                MB_LOG_WARNING("No response from modem (or read error)");
            }

            printf("[INFO] === MODEM_AUTOANSWER command completed (%s mode) ===\n", mode_name);
            fflush(stdout);
            MB_LOG_INFO("=== MODEM_AUTOANSWER command completed (%s mode) ===", mode_name);
        } else {
            printf("[DEBUG] Autoanswer command IS EMPTY for %s mode, skipping execution\n", mode_name);
            fflush(stdout);
            MB_LOG_WARNING("Autoanswer command IS EMPTY for %s mode", mode_name);
        }

        /* Wait for all pending responses and flush buffers before starting threads */
        printf("[INFO] Waiting for modem to settle (1 second)...\n");
        fflush(stdout);
        usleep(1000000);  /* 1 second */

        /* Drain any remaining responses from initialization commands */
        /* IMPORTANT: Process ALL hardware messages during draining (modem_sample pattern) */
        /* This includes: RING, CONNECT, NO CARRIER, etc. */
        unsigned char drain_buf[SMALL_BUFFER_SIZE];
        ssize_t drained;
        int drain_attempts = 0;
        printf("[DEBUG] ===== ENTERING DRAINING LOOP =====\n");
        fflush(stdout);
        do {
            printf("[DEBUG] Draining iteration %d: calling serial_read()...\n", drain_attempts);
            fflush(stdout);
            drained = serial_read(&ctx->serial, drain_buf, sizeof(drain_buf));
            printf("[DEBUG] serial_read() returned: %zd bytes\n", drained);
            fflush(stdout);
            if (drained > 0) {
                printf("[INFO] Draining initialization responses (%zd bytes): [%.*s]\n",
                       drained, (int)drained, drain_buf);
                fflush(stdout);
                MB_LOG_DEBUG("Drained %zd bytes: [%.*s]", drained, (int)drained, drain_buf);

                /* Process ALL hardware modem messages during drain phase (modem_sample pattern) */
                /* This handles RING, CONNECT, NO CARRIER that may arrive during initialization */
                bool msg_handled = modem_process_hardware_message(&ctx->modem, (char *)drain_buf, drained);

                if (msg_handled) {
                    printf("[INFO] Hardware message processed during drain phase\n");
                    fflush(stdout);
                    MB_LOG_INFO("Hardware message processed during drain phase");

                    /* Check modem state after message processing */
                    printf("[DEBUG] After hardware message: modem_state=%d, online=%d\n",
                           ctx->modem.state, ctx->modem.online);
                    fflush(stdout);
                    printf("[DEBUG] About to increment drain_attempts (current=%d)\n", drain_attempts);
                    fflush(stdout);
                }

                printf("[DEBUG] Before increment: drain_attempts=%d\n", drain_attempts);
                fflush(stdout);
                drain_attempts++;
                printf("[DEBUG] After increment: drain_attempts=%d\n", drain_attempts);
                fflush(stdout);
            }
            printf("[DEBUG] Draining loop iteration %d: drained=%zd bytes\n", drain_attempts, drained);
            fflush(stdout);
            printf("[DEBUG] About to sleep 100ms before next iteration\n");
            fflush(stdout);
            usleep(100000);  /* 100ms between drain attempts */
            printf("[DEBUG] Sleep complete, checking while condition: drained=%zd, drain_attempts=%d\n",
                   drained, drain_attempts);
            fflush(stdout);
        } while (drained > 0 && drain_attempts < 10);

        printf("[INFO] ===== DRAINING LOOP EXITED =====\n");
        fflush(stdout);
        printf("[INFO] Buffer drain complete (%d attempts)\n", drain_attempts);
        fflush(stdout);
        MB_LOG_INFO("Initialization complete, buffers drained");

        /* === Verify modem configuration for RING detection === */
        int s0_value = ctx->modem.settings.s_registers[SREG_AUTO_ANSWER];
        printf("[INFO] === MODEM CONFIGURATION SUMMARY ===\n");
        printf("[INFO]   Auto-answer mode: %s (MODEM_AUTOANSWER_MODE=%d)\n",
               (ctx->config->modem_autoanswer_mode == 0) ? "SOFTWARE" : "HARDWARE",
               ctx->config->modem_autoanswer_mode);
        printf("[INFO]   S0 register value: %d\n", s0_value);
        if (s0_value == 0) {
            printf("[INFO]   RING detection: Software will send ATA after 2 RINGs\n");
        } else {
            printf("[INFO]   RING detection: Hardware modem will auto-answer after %d rings\n", s0_value);
        }
        printf("[INFO] === Ready to monitor for RING signals ===\n");
        fflush(stdout);

        MB_LOG_INFO("Modem configuration: S0=%d (mode=%s)", s0_value,
                   (s0_value == 0) ? "SOFTWARE" : "HARDWARE");
        MB_LOG_INFO("Serial port opened successfully: %s",
                   ctx->config->serial_port);
    } else {
        /* Serial port not available - will retry later */
        printf("[DEBUG] Serial port open FAILED, entering retry mode\n");
        fflush(stdout);
        ctx->serial_ready = false;
        ctx->modem_ready = false;
        ctx->last_serial_retry = time(NULL);

        MB_LOG_WARNING("Serial port not available: %s (will retry every %d seconds)",
                      ctx->config->serial_port, ctx->serial_retry_interval);
    }

    /* Open data log if enabled (independent of serial) */
    if (ctx->config->data_log_enabled) {
        int ret_log = datalog_open(&ctx->datalog, ctx->config->data_log_file);
        if (ret_log == SUCCESS) {
            datalog_session_start(&ctx->datalog);
            MB_LOG_INFO("Data logging enabled: %s", ctx->config->data_log_file);
        } else {
            MB_LOG_WARNING("Failed to open data log, continuing without logging");
        }
    }

    /* Configure echo functionality based on config (Level 1 only) */
#ifndef ENABLE_LEVEL2
    if (ctx->config->echo_enabled) {
        MB_LOG_INFO("Enabling echo functionality (Level 1)");
        echo_enable(&ctx->echo, ctx->config->echo_immediate,
                   ctx->config->echo_first_delay, ctx->config->echo_min_interval);
        echo_set_prefix(&ctx->echo, ctx->config->echo_prefix);
        echo_set_transmission(&ctx->echo, 1000, 3, 100);  /* 1s timeout, 3 retries, 100ms delay */
    } else {
        MB_LOG_INFO("Echo functionality disabled (Level 1)");
        echo_disable(&ctx->echo);
    }
#else
    MB_LOG_DEBUG("Echo functionality disabled for Level 2/3");
#endif

    ctx->state = STATE_IDLE;
    ctx->running = true;

    /* Start threads (multithread mode) */
    ctx->thread_running = true;

#ifdef ENABLE_LEVEL3
    /* === LEVEL 3 MODE: Create both Serial and Telnet threads === */
    printf("[INFO] Creating Serial/Modem thread (Level 3 - Part 1)...\n");
    fflush(stdout);
    MB_LOG_INFO("Creating Serial/Modem thread (Level 3 - Part 1)...");

    int ret_serial = pthread_create(&ctx->serial_thread, NULL, serial_modem_thread_func, ctx);
    if (ret_serial != 0) {
        printf("[ERROR] Failed to create serial thread: %s\n", strerror(ret_serial));
        fflush(stdout);
        MB_LOG_ERROR("Failed to create serial thread: %s", strerror(ret_serial));
        ctx->thread_running = false;
        ctx->running = false;
        return ERROR_GENERAL;
    }
    printf("[INFO] Level 1 serial thread created successfully (pthread_id=%lu)\n", (unsigned long)ctx->serial_thread);
    fflush(stdout);
    MB_LOG_INFO("Level 1 serial thread created successfully");

    printf("[INFO] Creating Telnet thread (Level 3 - Part 2)...\n");
    fflush(stdout);
    MB_LOG_INFO("Creating Telnet thread (Level 3 - Part 2)...");

    int ret_telnet = pthread_create(&ctx->telnet_thread, NULL, telnet_thread_func, ctx);
    if (ret_telnet != 0) {
        printf("[ERROR] Failed to create telnet thread: %s\n", strerror(ret_telnet));
        fflush(stdout);
        MB_LOG_ERROR("Failed to create telnet thread: %s", strerror(ret_telnet));

        /* Clean up serial thread since telnet failed */
        pthread_cancel(ctx->serial_thread);
        pthread_join(ctx->serial_thread, NULL);

        ctx->thread_running = false;
        ctx->running = false;
        return ERROR_GENERAL;
    }
    printf("[INFO] Level 2 telnet thread created successfully (pthread_id=%lu)\n", (unsigned long)ctx->telnet_thread);
    fflush(stdout);
    MB_LOG_INFO("Level 2 telnet thread created successfully");

    printf("[INFO] Level 3: Both Level 1 (serial) and Level 2 (telnet) threads created successfully\n");
    fflush(stdout);
    MB_LOG_INFO("Level 3: Both Level 1 (serial) and Level 2 (telnet) threads created successfully");

#else /* ENABLE_LEVEL3 not defined */

#ifdef ENABLE_LEVEL2
    /* === LEVEL 2 MODE: Create Telnet thread only === */
    printf("[INFO] Creating Telnet thread (Level 2)...\n");
    fflush(stdout);
    MB_LOG_INFO("Creating Telnet thread (Level 2)...");
    int ret_thread = pthread_create(&ctx->telnet_thread, NULL, telnet_thread_func, ctx);
    if (ret_thread != 0) {
        printf("[ERROR] Failed to create telnet thread: %s\n", strerror(ret_thread));
        fflush(stdout);
        MB_LOG_ERROR("Failed to create telnet thread: %s", strerror(ret_thread));
        ctx->thread_running = false;
        ctx->running = false;
        return ERROR_GENERAL;
    }
    printf("[INFO] Level 2 telnet thread created successfully (pthread_id=%lu)\n", (unsigned long)ctx->telnet_thread);
    fflush(stdout);
    MB_LOG_INFO("Level 2 telnet thread created successfully");
#else
    /* === LEVEL 1 MODE: Create Serial thread only === */
    printf("[INFO] Creating Serial/Modem thread (Level 1)...\n");
    fflush(stdout);
    MB_LOG_INFO("Creating Serial/Modem thread (Level 1)...");
    int ret_thread = pthread_create(&ctx->serial_thread, NULL, serial_modem_thread_func, ctx);
    if (ret_thread != 0) {
        printf("[ERROR] Failed to create serial thread: %s\n", strerror(ret_thread));
        fflush(stdout);
        MB_LOG_ERROR("Failed to create serial thread: %s", strerror(ret_thread));
        ctx->thread_running = false;
        ctx->running = false;
        return ERROR_GENERAL;
    }
    printf("[INFO] Level 1 serial thread created successfully (pthread_id=%lu)\n", (unsigned long)ctx->serial_thread);
    fflush(stdout);
    MB_LOG_INFO("Level 1 serial thread created successfully");
#endif /* ENABLE_LEVEL2 */

#endif /* ENABLE_LEVEL3 */

#ifdef ENABLE_LEVEL3
    /* Initialize Level 3 system */
    printf("[INFO] Initializing Level 3 system...\n");
    fflush(stdout);
    int init_ret = bridge_init_level3(ctx);
    if (init_ret != SUCCESS) {
        printf("[WARNING] Failed to initialize Level 3 system: %d\n", init_ret);
        fflush(stdout);
        MB_LOG_WARNING("Failed to initialize Level 3 system: %d", init_ret);
    } else {
        printf("[INFO] Level 3 system initialized, level3_enabled=%d\n", ctx->level3_enabled);
        fflush(stdout);
    }

    /* Start Level 3 system if enabled */
    if (ctx->level3_enabled) {
        printf("[INFO] Creating Level 3 management thread...\n");
        fflush(stdout);
        MB_LOG_INFO("Creating Level 3 management thread...");

        int ret_l3 = bridge_start_level3(ctx);
        if (ret_l3 != SUCCESS) {
            printf("[WARNING] Failed to start Level 3 system: %d\n", ret_l3);
            fflush(stdout);
            MB_LOG_WARNING("Failed to start Level 3 system: %d", ret_l3);
            /* Continue without Level 3 - not critical */
        } else {
            printf("[INFO] Level 3 management thread created successfully\n");
            fflush(stdout);
            MB_LOG_INFO("Level 3 management thread created successfully");
        }
    } else {
        printf("[WARNING] Level 3 enabled flag is false, not starting Level 3 thread\n");
        fflush(stdout);
    }
#endif

    printf("[DEBUG] bridge_start() about to check serial_ready status: %d\n", ctx->serial_ready);
    fflush(stdout);

    if (ctx->serial_ready) {
        printf("[DEBUG] Serial IS ready\n");
        fflush(stdout);
        MB_LOG_INFO("Bridge started (READY state), waiting for modem commands");
    } else {
        printf("[DEBUG] Serial IS NOT ready\n");
        fflush(stdout);
        MB_LOG_INFO("Bridge started (DISCONNECTED state), waiting for serial port");
    }

    printf("[DEBUG] bridge_start() returning SUCCESS\n");
    fflush(stdout);

    return SUCCESS;  /* Always return success! */
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

    /* Signal threads to stop */
    ctx->thread_running = false;

    /* Wake up any blocked threads */
#ifdef ENABLE_LEVEL2
    pthread_cond_broadcast(&ctx->ts_serial_to_telnet_buf.cond_not_empty);
    pthread_cond_broadcast(&ctx->ts_serial_to_telnet_buf.cond_not_full);
    pthread_cond_broadcast(&ctx->ts_telnet_to_serial_buf.cond_not_empty);
    pthread_cond_broadcast(&ctx->ts_telnet_to_serial_buf.cond_not_full);
#else
    /* Level 1: Telnet buffers not available */
#endif

#ifdef ENABLE_LEVEL3
    /* Stop Level 3 system first */
    MB_LOG_INFO("Stopping Level 3 pipeline management");
    bridge_stop_level3(ctx);
#endif

  #ifdef ENABLE_LEVEL3
    /* === LEVEL 3 MODE: Wait for both Serial and Telnet threads === */
    MB_LOG_INFO("Waiting for Level 1 serial thread to exit...");
    pthread_join(ctx->serial_thread, NULL);
    MB_LOG_INFO("Serial/Modem thread (Level 1) exited");

    MB_LOG_INFO("Waiting for Level 2 telnet thread to exit...");
    pthread_join(ctx->telnet_thread, NULL);
    MB_LOG_INFO("Telnet thread (Level 2) exited");

    /* Disconnect telnet if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }
#else

#ifdef ENABLE_LEVEL2
    MB_LOG_INFO("Waiting for Level 2 telnet thread to exit...");

    /* Wait for Level 2 telnet thread to exit */
    pthread_join(ctx->telnet_thread, NULL);
    MB_LOG_INFO("Telnet thread (Level 2) exited");

    /* Disconnect telnet if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }
#else
    MB_LOG_INFO("Waiting for Level 1 serial thread to exit...");

    /* Wait for Level 1 serial thread to exit */
    pthread_join(ctx->serial_thread, NULL);
    MB_LOG_INFO("Serial/Modem thread (Level 1) exited");
#endif

#endif /* ENABLE_LEVEL3 */

    /* Hang up modem (if serial is ready) */
    if (ctx->modem_ready && modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
    }

    /* Close serial port (if ready) */
    if (ctx->serial_ready) {
        serial_close(&ctx->serial);
    }

    /* Unlock serial port (modem_sample approach) */
    serial_unlock_port();
    MB_LOG_INFO("Serial port unlocked");

    /* Close data log */
    if (datalog_is_enabled(&ctx->datalog)) {
        datalog_close(&ctx->datalog);
    }

    /* Cleanup thread resources */
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->modem_mutex);
#ifdef ENABLE_LEVEL2
    ts_cbuf_destroy(&ctx->ts_serial_to_telnet_buf);
    ts_cbuf_destroy(&ctx->ts_telnet_to_serial_buf);
#endif

    /* Print statistics */
    bridge_print_stats(ctx);

    MB_LOG_INFO("Bridge stopped (all threads cleaned up)");

    return SUCCESS;
}

#ifdef ENABLE_LEVEL2
/**
 * Handle modem connection establishment and initiate telnet connection (Level 2 only)
 * Called when hardware modem is ONLINE (CONNECT message received)
 */
int bridge_handle_modem_connect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("=== Hardware modem CONNECT received - starting telnet connection ===");

    /* Hardware modem is already ONLINE at this point */
    /* modem->state should be MODEM_STATE_ONLINE, modem->online should be true */

    /* Connect to telnet server */
    MB_LOG_INFO("Connecting to telnet server %s:%d",
               ctx->config->telnet_host, ctx->config->telnet_port);
    int ret = telnet_connect(&ctx->telnet, ctx->config->telnet_host, ctx->config->telnet_port);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to connect to telnet server");
        /* Hardware modem is online but telnet failed - send NO CARRIER and hang up */
        modem_hangup(&ctx->modem);
        modem_send_no_carrier(&ctx->modem);
        ctx->state = STATE_IDLE;
        return ret;
    }

    MB_LOG_INFO("Telnet connection established successfully");

    /* Ensure modem is in online state */
    if (!modem_is_online(&ctx->modem)) {
        MB_LOG_INFO("Modem not online yet, setting online mode");
        modem_go_online(&ctx->modem);
    }

    /* Synchronize echo settings between modem and telnet
     * Note: Telnet option negotiation happens asynchronously,
     * so echo mode will be updated as negotiations complete */
    bridge_sync_echo_mode(ctx);

    /* Send CONNECT message to modem (if not already sent by hardware) */
    modem_send_connect(&ctx->modem, ctx->config->baudrate_value);

    /* Both connections established - start data transfer */
    ctx->state = STATE_CONNECTED;
    ctx->connection_start_time = time(NULL);

  
    MB_LOG_INFO("=== Bridge connection FULLY established - data transfer ready ===");

    return SUCCESS;
}
#endif

#ifdef ENABLE_LEVEL2
/**
 * Synchronize modem echo with telnet echo mode (Level 2 only)
 */
static void bridge_sync_echo_mode(bridge_ctx_t *ctx)
{
    if (ctx == NULL || !telnet_is_connected(&ctx->telnet)) {
        return;
    }

    /* If server will echo, disable modem local echo to prevent double echo */
    if (ctx->telnet.remote_options[TELOPT_ECHO]) {
        if (ctx->modem.settings.echo) {
            MB_LOG_INFO("Server WILL ECHO - disabling modem local echo to prevent double echo");
            ctx->modem.settings.echo = false;
        }
    }
    /* If server won't echo, keep modem echo setting (from ATE command) */
    else {
        MB_LOG_INFO("Server WONT ECHO - using modem echo setting (ATE command)");
    }
}
#endif

#ifdef ENABLE_LEVEL2
/**
 * Reinitialize modem to initial state
 * This function is called after connection termination to reset modem
 * to the same state as program startup
 */
static int bridge_reinitialize_modem(bridge_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->modem_ready) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Reinitializing modem to initial state");

    /* Reinitialize modem structure */
    modem_init(&ctx->modem, &ctx->serial);

    /* Execute MODEM_INIT_COMMAND if configured */
    if (ctx->config->modem_init_command[0] != '\0') {
        char cmd_buf[LINE_BUFFER_SIZE * 2];
        unsigned char response[SMALL_BUFFER_SIZE];

        MB_LOG_INFO("Executing MODEM_INIT_COMMAND for reinitialization");
        MB_LOG_DEBUG("Command: %s", ctx->config->modem_init_command);

        /* Process commands separated by semicolons */
        char *cmd_copy = strdup(ctx->config->modem_init_command);
        char *token = strtok(cmd_copy, ";");

        while (token != NULL) {
            /* Skip leading/trailing spaces */
            while (*token == ' ') token++;
            char *end = token + strlen(token) - 1;
            while (end > token && *end == ' ') *end-- = '\0';

            if (*token != '\0') {
                /* Build command with AT prefix if needed and \r\n suffix */
                if (strncasecmp(token, "AT", 2) == 0) {
                    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", token);
                } else {
                    snprintf(cmd_buf, sizeof(cmd_buf), "AT%s\r\n", token);
                }

                /* Send command to modem */
                MB_LOG_DEBUG("Sending reinit command: %s", token);
                ssize_t sent = serial_write(&ctx->serial,
                                (const unsigned char *)cmd_buf,
                                strlen(cmd_buf));

                if (sent > 0) {
                    /* Wait for response */
                    usleep(200000);  /* 200ms between commands */
                    ssize_t resp_len = serial_read(&ctx->serial, response, sizeof(response) - 1);

                    if (resp_len > 0) {
                        response[resp_len] = '\0';
                        MB_LOG_DEBUG("Reinit response: %.*s", (int)resp_len, response);
                    }
                }
            }

            token = strtok(NULL, ";");
        }

        free(cmd_copy);
        MB_LOG_INFO("MODEM_INIT_COMMAND reinitialization completed");

        /* Small delay after initialization */
        usleep(500000);  /* 500ms */
    }

    /* Re-apply auto-answer settings */
    const char *autoanswer_cmd = NULL;
    const char *mode_name = NULL;

    if (ctx->config->modem_autoanswer_mode == 0) {
        /* SOFTWARE mode: S0=0, manual ATA required */
        autoanswer_cmd = ctx->config->modem_autoanswer_software_command;
        mode_name = "SOFTWARE";
    } else {
        /* HARDWARE mode: S0>0, automatic answer */
        autoanswer_cmd = ctx->config->modem_autoanswer_hardware_command;
        mode_name = "HARDWARE";
    }

    if (autoanswer_cmd && autoanswer_cmd[0] != '\0') {
        char cmd_buf[LINE_BUFFER_SIZE * 2];  /* Double size to prevent truncation */

        /* Build command with AT prefix if needed */
        if (strncasecmp(autoanswer_cmd, "AT", 2) == 0) {
            snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", autoanswer_cmd);
        } else {
            snprintf(cmd_buf, sizeof(cmd_buf), "AT%s\r\n", autoanswer_cmd);
        }

        MB_LOG_INFO("Setting auto-answer mode: %s (%s)", mode_name, autoanswer_cmd);
        ssize_t sent = serial_write(&ctx->serial,
                        (const unsigned char *)cmd_buf,
                        strlen(cmd_buf));

        if (sent > 0) {
            unsigned char response[SMALL_BUFFER_SIZE];
            usleep(200000);  /* 200ms wait */
            ssize_t resp_len = serial_read(&ctx->serial, response, sizeof(response) - 1);
            if (resp_len > 0) {
                response[resp_len] = '\0';
                MB_LOG_DEBUG("Auto-answer response: %.*s", (int)resp_len, response);
            }
        }
    }

    MB_LOG_INFO("Modem reinitialization complete - ready for new connections");
    return SUCCESS;
}
#endif

#ifdef ENABLE_LEVEL2
/**
 * Handle modem disconnection (Level 2 only)
 */
int bridge_handle_modem_disconnect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Modem disconnected - cleaning up connection");

    /* Disconnect telnet if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }

    /* Send NO CARRIER */
    modem_send_no_carrier(&ctx->modem);

    /* Reinitialize modem to initial state */
    int ret = bridge_reinitialize_modem(ctx);
    if (ret != SUCCESS) {
        MB_LOG_WARNING("Failed to reinitialize modem after modem disconnect: %d", ret);
        /* Continue anyway - don't fail the disconnect handling */
    }

    ctx->state = STATE_IDLE;

    MB_LOG_INFO("Modem returned to initial state - ready for new connection");

    return SUCCESS;
}

/**
 * Handle telnet connection establishment (Level 2 only)
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
 * Handle telnet disconnection (Level 2 only)
 */
int bridge_handle_telnet_disconnect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Telnet disconnected - initiating modem reinitialization");

    /* Hang up modem first */
    if (modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
        modem_send_no_carrier(&ctx->modem);
    }

    /* Reinitialize modem to initial state */
    int ret = bridge_reinitialize_modem(ctx);
    if (ret != SUCCESS) {
        MB_LOG_WARNING("Failed to reinitialize modem after telnet disconnect: %d", ret);
        /* Continue anyway - don't fail the disconnect handling */
    }

    ctx->state = STATE_IDLE;

    MB_LOG_INFO("Modem returned to initial state - ready for new connection");

    return SUCCESS;
}
#endif

/* ========== Serial Processing Functions ========== */
/*
 * The following Level 1 serial processing functions have been moved to level1_serial.c:
 * - bridge_process_serial_data() (Level 1 version)
 * - level1_process_command_mode()
 * - level1_process_online_mode()
 * - level1_log_serial_data()
 * - level1_handle_serial_error()
 * - level1_check_hardware_messages()
 * - bridge_handle_modem_connect_level1()
 * - bridge_handle_modem_disconnect_level1()
 * - level1_serial_init()
 * - level1_serial_cleanup()
 *
 * These functions are now available through #include "level1_serial.h"
 * when ENABLE_LEVEL1 is defined.
 */

#if defined(ENABLE_LEVEL2) && !defined(ENABLE_LEVEL1)
/* Level 2-only mode: bridge_process_serial_data wraps the level2 version */
int bridge_process_serial_data(bridge_ctx_t *ctx)
{
    /* Level 2 should use bridge_process_serial_data_level2() */
    return bridge_process_serial_data_level2(ctx);
}
#endif

#ifdef ENABLE_LEVEL2
/**
 * Process data from serial port - Level 2 exclusive implementation
 * Handles serial I/O errors and transitions to DISCONNECTED state
 * Focuses on telnet bridging functionality with minimal Level 1 sharing
 */
int bridge_process_serial_data_level2(bridge_ctx_t *ctx)
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
    struct timeval tv_before, tv_after;
    gettimeofday(&tv_before, NULL);

    printf("[DEBUG] [Level 2] bridge_process_serial_data: About to call serial_read()\n");
    fflush(stdout);
    n = serial_read(&ctx->serial, buf, sizeof(buf));

    gettimeofday(&tv_after, NULL);
    long elapsed_us = (tv_after.tv_sec - tv_before.tv_sec) * 1000000L +
                      (tv_after.tv_usec - tv_before.tv_usec);

    printf("[DEBUG] [Level 2] bridge_process_serial_data: serial_read() returned %zd (took %ld us)\n", n, elapsed_us);
    fflush(stdout);

    if (n > 0) {
        /* Log ALL serial data with hex dump for debugging */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

        printf("[INFO] [%s] [Level 2] <<< Serial RX: %zd bytes >>>\n", timestamp, n);
        fflush(stdout);
        MB_LOG_INFO("[Level 2] <<< Serial RX: %zd bytes >>>", n);
        MB_LOG_DEBUG("  HEX: %.*s", (int)n, buf);

        /* Print as hex for better visibility */
        char hex_str[BUFFER_SIZE * 3 + 1];
        for (ssize_t i = 0; i < n && i < 64; i++) {
            snprintf(hex_str + i*3, 4, "%02X ", (unsigned char)buf[i]);
        }
        printf("[INFO] [%s] [Level 2]   DATA: [%s]\n", timestamp, hex_str);
        fflush(stdout);
        MB_LOG_INFO("[Level 2]  DATA: [%s]", hex_str);
    } else if (n == 0) {
        printf("[DEBUG] [Level 2] bridge_process_serial_data: serial_read() returned 0 (no data or EAGAIN)\n");
        fflush(stdout);
        MB_LOG_DEBUG("[Level 2] serial_read() returned 0 (no data available)");
    }

    if (n < 0) {
        /* Serial I/O error detected - transition to DISCONNECTED state */
        MB_LOG_ERROR("[Level 2] Serial I/O error detected: %s", strerror(errno));

        /* Close telnet connection if active (ONLINE state) */
        if (telnet_is_connected(&ctx->telnet)) {
            MB_LOG_INFO("[Level 2] Closing telnet connection due to serial port error");
            telnet_disconnect(&ctx->telnet);

            /* Send NO CARRIER to modem if online */
            if (modem_is_online(&ctx->modem)) {
                modem_send_no_carrier(&ctx->modem);
            }
        }

        /* Close serial port */
        serial_close(&ctx->serial);

        /* Transition to DISCONNECTED state */
        ctx->serial_ready = false;
        ctx->modem_ready = false;
        ctx->last_serial_retry = time(NULL);
        ctx->state = STATE_IDLE;

        MB_LOG_WARNING("[Level 2] Transitioned to DISCONNECTED state (will retry in %d seconds)",
                      ctx->serial_retry_interval);

        return ERROR_IO;
    }

    if (n == 0) {
        /* No data */
        return SUCCESS;
    }

    /* Log data from modem */
    datalog_write(&ctx->datalog, DATALOG_DIR_FROM_MODEM, buf, n);

    /* First, check for hardware modem unsolicited messages (RING, CONNECT, NO CARRIER) */
    /* This is critical for real hardware modems that send these messages */
    bool hardware_msg_handled = modem_process_hardware_message(&ctx->modem, (char *)buf, n);

    /* Check for state changes after hardware message processing */
    modem_state_t current_state = modem_get_state(&ctx->modem);

    if (current_state == MODEM_STATE_CONNECTING) {
        /* Modem is connecting (ATA sent, waiting for CONNECT from hardware) */
        MB_LOG_INFO("[Level 2] Modem in CONNECTING state - waiting for hardware CONNECT response");
        /* Do NOT start telnet connection yet! Wait for hardware modem CONNECT */
        return SUCCESS;
    } else if (current_state == MODEM_STATE_ONLINE) {
        /* === LEVEL 2: Hardware modem ONLINE - initiate telnet connection === */
        MB_LOG_INFO("[Level 2] Hardware modem ONLINE - initiating telnet bridge");

        /* Call Level 2 specific modem connect handler */
        int ret = bridge_handle_modem_connect(ctx);
        if (ret != SUCCESS) {
            MB_LOG_ERROR("[Level 2] Failed to establish telnet connection");
            return ret;
        }

        /* Update state to indicate connection is active */
        ctx->state = STATE_CONNECTED;
        ctx->connection_start_time = time(NULL);

        return SUCCESS;
    } else if (current_state == MODEM_STATE_DISCONNECTED) {
        MB_LOG_INFO("[Level 2] Modem state changed to DISCONNECTED");
        bridge_handle_modem_disconnect(ctx);
        return SUCCESS;
    }

    /* Process through modem layer only if not a hardware message */
    if (!hardware_msg_handled && !modem_is_online(&ctx->modem)) {
        /* In command mode - let modem process the input */
        modem_process_input(&ctx->modem, (char *)buf, n);
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

    /* === LEVEL 2: Transfer actual data to telnet - DISABLED === */
    if (!telnet_is_connected(&ctx->telnet)) {
        MB_LOG_DEBUG("[Level 2] No telnet connection - data discarded");
        return SUCCESS;
    }

    /* Filter ANSI sequences for telnet compatibility - DISABLED */
    ansi_filter_modem_to_telnet(buf, consumed, filtered_buf, sizeof(filtered_buf),
                                &filtered_len, &ctx->ansi_filter_state);

    if (filtered_len == 0) {
        MB_LOG_DEBUG("[Level 2] No data after ANSI filtering");
        return SUCCESS;
    }

    /* Prepare for telnet (escape IAC bytes) - DISABLED */
    telnet_prepare_output(&ctx->telnet, filtered_buf, filtered_len,
                         telnet_buf, sizeof(telnet_buf), &telnet_len);

    if (telnet_len == 0) {
        MB_LOG_DEBUG("[Level 2] No data after telnet preparation");
        return SUCCESS;
    }

    /* Log data to telnet (after IAC escaping) - DISABLED */
    datalog_write(&ctx->datalog, DATALOG_DIR_TO_TELNET, telnet_buf, telnet_len);

    /* Send to telnet - DISABLED */
    ssize_t sent = telnet_send(&ctx->telnet, telnet_buf, telnet_len);
    if (sent > 0) {
        ctx->bytes_serial_to_telnet += sent;
        MB_LOG_DEBUG("[Level 2] Sent %zd bytes to telnet", sent);
    } else if (sent < 0) {
        MB_LOG_ERROR("[Level 2] Failed to send data to telnet");
        bridge_handle_telnet_disconnect(ctx);
        return ERROR_IO;
    }

    return SUCCESS;
}

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
#endif

#ifdef ENABLE_LEVEL2
/**
 * Transfer data from telnet to serial (Level 2 only)
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
#endif

/**
 * Main bridge loop (multithread mode)
 * In multithread mode, this function just sleeps and allows signal handling.
 * The actual I/O is handled by serial_modem_thread_func() and telnet_thread_func().
 */
int bridge_run(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->running) {
        return ERROR_GENERAL;
    }

    /* In multithread mode, threads handle all I/O operations.
     * Main loop just sleeps to allow signal handling and config reload. */
    usleep(100000);  /* 100ms - allows responsive signal handling */

    
    /* Check if serial port needs retry (for auto-reconnect) */
    if (!ctx->serial_ready) {
        time_t now = time(NULL);

        /* Check every 10 seconds */
        if (now - ctx->last_serial_retry >= ctx->serial_retry_interval) {
            ctx->last_serial_retry = now;
            ctx->serial_retry_count++;

            /* Check if device file exists */
            if (access(ctx->config->serial_port, F_OK) == 0) {
                MB_LOG_INFO("Serial port device detected (attempt #%d): %s",
                           ctx->serial_retry_count, ctx->config->serial_port);

                /* Try to open serial port */
                int ret = serial_open(&ctx->serial, ctx->config->serial_port, ctx->config);
                if (ret == SUCCESS) {
                    ctx->serial_ready = true;

                    /* Initialize modem (no health check) */
                    modem_init(&ctx->modem, &ctx->serial);

  #ifdef ENABLE_LEVEL3
                    /* Register DCD event callback for Level 3 integration */
                    /* This callback works in all levels (1, 2, 3) for DCD monitoring */
                    int dcd_ret = modem_set_dcd_event_callback(&ctx->modem,
                                                              (int (*)(void *, bool))bridge_handle_dcd_event,
                                                              ctx);
                    if (dcd_ret == SUCCESS) {
                        MB_LOG_DEBUG("DCD event callback re-registered after serial retry");
                    } else {
                        MB_LOG_WARNING("Failed to re-register DCD event callback after retry: %d", dcd_ret);
                    }
#else
                    /* Level 3 not available - DCD callback registration skipped */
                    MB_LOG_DEBUG("Level 3 not enabled, DCD event callback registration skipped after serial retry");
#endif

                    ctx->modem_ready = true;

                    MB_LOG_INFO("Transitioned to READY state after %d attempts",
                               ctx->serial_retry_count);
                    MB_LOG_INFO("Serial port connected: %s", ctx->config->serial_port);

                    /* Reset retry counter */
                    ctx->serial_retry_count = 0;
                } else {
                    MB_LOG_DEBUG("Serial port open failed (attempt #%d): %s",
                                ctx->serial_retry_count, strerror(errno));
                }
            } else {
                MB_LOG_DEBUG("Serial port device not found (attempt #%d): %s",
                            ctx->serial_retry_count, ctx->config->serial_port);
            }
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
#ifdef ENABLE_LEVEL2
    MB_LOG_INFO("Serial -> Telnet: %llu bytes",
                (unsigned long long)ctx->bytes_serial_to_telnet);
    MB_LOG_INFO("Telnet -> Serial: %llu bytes",
                (unsigned long long)ctx->bytes_telnet_to_serial);
#else
    MB_LOG_INFO("Level 1 mode: Telnet statistics not available");
#endif

    if (ctx->connection_start_time > 0) {
        time_t duration = time(NULL) - ctx->connection_start_time;
        MB_LOG_INFO("Connection duration: %ld seconds", (long)duration);
    }

#ifdef ENABLE_LEVEL3
    if (ctx->level3_enabled && ctx->level3 != NULL) {
        MB_LOG_INFO("--- Level 3 Statistics ---");
        l3_print_stats((l3_context_t*)ctx->level3);
    }
#endif

    MB_LOG_INFO("========================");
}


/* ========== Multithread Mode: Thread Functions ========== */

#if !defined(ENABLE_LEVEL2) || defined(ENABLE_LEVEL3)
/**
 * Serial/Modem thread function - Level 1
 * Handles:
 * - Serial health check (at startup)
 * - Modem initialization verification
 * - Serial I/O (reading from serial port)
 * - Modem command processing (AT commands)
 * - Timestamp transmission every 3 seconds when modem is online
 *
 * In Level 3 mode, this runs alongside the telnet thread for dual-pipeline operation
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
#endif

#ifdef ENABLE_LEVEL2
/**
 * Telnet thread function - Level 2 only
 * Handles:
 * - Telnet I/O (reading from telnet server)
 * - IAC protocol processing
 * - Telnet → Serial data buffering
 * - Serial → Telnet data transmission
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
#endif

/* ========== Level 3 Integration ========== */

#ifdef ENABLE_LEVEL3

/**
 * Initialize Level 3 pipeline system
 */
int bridge_init_level3(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Initializing Level 3 pipeline system");

    /* Allocate Level 3 context */
    ctx->level3 = malloc(sizeof(l3_context_t));
    if (ctx->level3 == NULL) {
        MB_LOG_ERROR("Failed to allocate memory for Level 3 context");
        return ERROR_GENERAL;
    }

    /* Initialize Level 3 context */
    int ret = l3_init((l3_context_t*)ctx->level3, ctx);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to initialize Level 3 context: %d", ret);
        free(ctx->level3);
        ctx->level3 = NULL;
        return ret;
    }

    /* Check if Level 3 should be enabled */
    ctx->level3_enabled = bridge_should_enable_level3(ctx);
    if (ctx->level3_enabled) {
        MB_LOG_INFO("Level 3 system enabled (dual pipeline mode)");
    } else {
        MB_LOG_INFO("Level 3 system disabled (falling back to legacy mode)");
    }

    return SUCCESS;
}

/**
 * Start Level 3 pipeline management
 */
int bridge_start_level3(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->level3_enabled) {
        MB_LOG_DEBUG("Level 3 not enabled, skipping startup");
        return SUCCESS;
    }

    MB_LOG_INFO("Starting Level 3 pipeline management");

    /* Start Level 3 system */
    int ret = l3_start((l3_context_t*)ctx->level3);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to start Level 3 system: %d", ret);
        return ret;
    }

    /* Create Level 3 management thread */
    ret = pthread_create(&ctx->level3_thread, NULL, bridge_level3_thread_func, ctx);
    if (ret != 0) {
        MB_LOG_ERROR("Failed to create Level 3 management thread: %s", strerror(ret));
        l3_stop((l3_context_t*)ctx->level3);
        return ERROR_GENERAL;
    }

    MB_LOG_INFO("Level 3 management thread started successfully");
    return SUCCESS;
}

/**
 * Stop Level 3 pipeline management
 */
int bridge_stop_level3(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->level3_enabled) {
        MB_LOG_DEBUG("Level 3 not enabled, skipping shutdown");
        return SUCCESS;
    }

    MB_LOG_INFO("Stopping Level 3 pipeline management");

    /* Stop Level 3 system */
    if (ctx->level3 != NULL) {
        l3_stop((l3_context_t*)ctx->level3);

        /* Wait for Level 3 thread to exit */
        if (pthread_join(ctx->level3_thread, NULL) != 0) {
            MB_LOG_WARNING("Failed to join Level 3 management thread");
        }

        /* Cleanup Level 3 resources */
        l3_cleanup((l3_context_t*)ctx->level3);
        free(ctx->level3);
        ctx->level3 = NULL;
    }

    MB_LOG_INFO("Level 3 pipeline management stopped");
    return SUCCESS;
}

/**
 * Check if Level 3 should be enabled based on configuration and system state
 */
bool bridge_should_enable_level3(bridge_ctx_t *ctx)
{
    if (ctx == NULL || ctx->config == NULL) {
        return false;
    }

    /* Check if Level 3 is explicitly enabled in configuration */
    /* Note: This assumes a config option like level3_enabled exists */
    /* For now, enable Level 3 when both Level 1 and Level 2 are available */

#ifndef ENABLE_LEVEL2
    /* Level 3 requires Level 2 (telnet) */
    MB_LOG_DEBUG("Level 3 disabled: Level 2 (telnet) not available");
    return false;
#endif

    /* Check system requirements */
    if (!ctx->serial_ready) {
        MB_LOG_DEBUG("Level 3 disabled: Serial port not ready");
        return false;
    }

    /* Enable Level 3 if all requirements are met */
    MB_LOG_DEBUG("Level 3 requirements met, enabling dual pipeline system");
    return true;
}

/**
 * Level 3 thread function
 * Manages dual pipeline system with fair scheduling and backpressure
 */
void *bridge_level3_thread_func(void *arg)
{
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;

    MB_LOG_INFO("[Level 3 Thread] Pipeline management thread started");

    while (ctx->thread_running && ctx->level3_enabled) {
        /* Level 3 main loop is handled by l3_management_thread_func */
        /* This thread serves as a wrapper and monitor */

        /* Check if Level 3 system is still healthy */
        if (ctx->level3 != NULL) {
            l3_context_t *l3_ctx = (l3_context_t*)ctx->level3;
            if (l3_ctx->level3_active) {
                /* Update system statistics */
                double utilization = l3_get_system_utilization(l3_ctx);
                if (utilization > 90.0) {
                    MB_LOG_WARNING("[Level 3] High system utilization: %.2f%%", utilization);
                }

                /* Check for pipeline switches */
                if (l3_ctx->total_pipeline_switches > 0 &&
                    l3_ctx->total_pipeline_switches % 1000 == 0) {
                    MB_LOG_INFO("[Level 3] Milestone: %llu pipeline switches completed",
                               (unsigned long long)l3_ctx->total_pipeline_switches);
                }
            }
        }

        /* Sleep to prevent busy waiting */
        usleep(1000000);  /* 1 second */
    }

    MB_LOG_INFO("[Level 3 Thread] Pipeline management thread exiting");
    return NULL;
}

/* ========== DCD Event Bridge Functions ========== */

/**
 * Handle DCD (Data Carrier Detect) state changes from modem
 * This function bridges DCD events between Level 1 (modem) and Level 3 (pipeline)
 * Called by modem.c when DCD state changes
 */
int bridge_handle_dcd_event(bridge_ctx_t *ctx, bool dcd_state)
{
    printf("[DEBUG-BRIDGE-DCD] bridge_handle_dcd_event() ENTRY: ctx=%p, dcd_state=%d\n", (void*)ctx, dcd_state);
    fflush(stdout);

    if (ctx == NULL) {
        printf("[ERROR-BRIDGE-DCD] ctx is NULL!\n");
        fflush(stdout);
        return ERROR_INVALID_ARG;
    }

    printf("[INFO-BRIDGE-DCD] Bridge received DCD event: %s\n", dcd_state ? "RISED (ON)" : "FELL (OFF)");
    fflush(stdout);
    MB_LOG_INFO("Bridge received DCD event: %s", dcd_state ? "RISED (ON)" : "FELL (OFF)");

    /* Store DCD state for reference */
    ctx->carrier_detected = dcd_state;

#ifdef ENABLE_LEVEL3
    printf("[DEBUG-BRIDGE-DCD] ENABLE_LEVEL3 is defined\n");
    fflush(stdout);
    printf("[DEBUG-BRIDGE-DCD] ctx->level3_enabled=%d, ctx->level3=%p\n", ctx->level3_enabled, ctx->level3);
    fflush(stdout);

    /* Forward DCD event to Level 3 pipeline management */
    if (ctx->level3_enabled && ctx->level3 != NULL) {
        printf("[INFO-BRIDGE-DCD] Level 3 is enabled and initialized - forwarding DCD event\n");
        fflush(stdout);
        l3_context_t *l3_ctx = (l3_context_t*)ctx->level3;

        if (dcd_state) {
            /* DCD rising edge - trigger pipeline activation */
            printf("[INFO-BRIDGE-DCD] Forwarding DCD rising edge to Level 3 pipeline\n");
            fflush(stdout);
            MB_LOG_INFO("Forwarding DCD rising edge to Level 3 pipeline");
            int ret = l3_on_dcd_rising(l3_ctx);
            printf("[INFO-BRIDGE-DCD] l3_on_dcd_rising() returned: %d\n", ret);
            fflush(stdout);
            if (ret != SUCCESS) {
                MB_LOG_WARNING("Failed to forward DCD rising edge to Level 3: %d", ret);
            }
        } else {
            /* DCD falling edge - trigger pipeline deactivation */
            printf("[INFO-BRIDGE-DCD] Forwarding DCD falling edge to Level 3 pipeline\n");
            fflush(stdout);
            MB_LOG_INFO("Forwarding DCD falling edge to Level 3 pipeline");
            int ret = l3_on_dcd_falling(l3_ctx);
            printf("[INFO-BRIDGE-DCD] l3_on_dcd_falling() returned: %d\n", ret);
            fflush(stdout);
            if (ret != SUCCESS) {
                MB_LOG_WARNING("Failed to forward DCD falling edge to Level 3: %d", ret);
            }
        }
    } else {
        printf("[WARNING-BRIDGE-DCD] Level 3 not enabled or not initialized (enabled=%d, level3=%p)\n",
               ctx->level3_enabled, ctx->level3);
        fflush(stdout);
        MB_LOG_DEBUG("Level 3 not enabled or not initialized, DCD event handled locally");
    }
#else
    printf("[WARNING-BRIDGE-DCD] ENABLE_LEVEL3 is NOT defined!\n");
    fflush(stdout);
    /* Level 3 not available - handle DCD locally */
    MB_LOG_DEBUG("Level 3 not compiled in, DCD event handled at bridge level");
#endif

    printf("[DEBUG-BRIDGE-DCD] bridge_handle_dcd_event() returning SUCCESS\n");
    fflush(stdout);
    return SUCCESS;
}

/**
 * Get current DCD state from bridge
 * Returns the last known DCD state
 */
bool bridge_get_dcd_state(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    return ctx->carrier_detected;
}

/**
 * Check if bridge should notify Level 3 of DCD events
 * Returns true if Level 3 is active and ready to receive DCD events
 */
bool bridge_should_notify_level3_dcd(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }

#ifdef ENABLE_LEVEL3
    return (ctx->level3_enabled && ctx->level3 != NULL);
#else
    return false;
#endif
}

#endif /* ENABLE_LEVEL3 */
