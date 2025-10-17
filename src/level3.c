/*
 * level3.c - Level 3 Pipeline Management Implementation for ModemBridge
 *
 * This module implements the dual pipeline system that manages data flow between
 * Level 1 (Serial/Modem) and Level 2 (Telnet) with half-duplex operation,
 * fair scheduling, and protocol-specific filtering.
 */

#include "level3.h"
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/* Global state for telnet connection attempts in CONNECTING state */
bool g_level3_connection_attempted = false;
time_t g_level3_last_attempt = 0;
bool g_level3_transition_logged = false;

/* Forward declaration of Hayes dictionary */
static const hayes_dictionary_t hayes_dictionary;

/* Internal helper functions - Forward declarations */
static void l3_update_pipeline_stats(l3_pipeline_t *pipeline, size_t bytes_processed, double processing_time);
static int l3_init_enhanced_scheduling(l3_context_t *l3_ctx);
static int l3_process_pipeline_with_quantum(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);
static void l3_update_latency_stats(l3_context_t *l3_ctx, l3_pipeline_direction_t direction, long long latency_ms);
static bool l3_is_direction_starving(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);
static int l3_calculate_optimal_quantum(l3_context_t *l3_ctx);
static int l3_update_fair_queue_weights(l3_context_t *l3_ctx);
static int l3_get_scheduling_statistics(l3_context_t *l3_ctx, l3_scheduling_stats_t *stats);
static const char *l3_get_direction_name(l3_pipeline_direction_t direction);
static int l3_process_serial_to_telnet_chunk(l3_context_t *l3_ctx);
static int l3_process_telnet_to_serial_chunk(l3_context_t *l3_ctx);

/* Enhanced scheduling with latency bound guarantee */
static int l3_enforce_latency_boundaries(l3_context_t *l3_ctx);
static int l3_detect_latency_violation(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);
static int l3_calculate_adaptive_quantum_with_latency(l3_context_t *l3_ctx);
static int l3_update_direction_priorities(l3_context_t *l3_ctx);
static long long l3_get_direction_wait_time(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);
static bool l3_should_force_direction_switch(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);

/* ========== Multibyte Character Handling ========== */

/**
 * Detect if byte is the start of a multibyte sequence
 * @param c Byte to check
 * @return true if multibyte start, false otherwise
 */
static bool l3_is_multibyte_start(unsigned char c)
{
    /* UTF-8 multibyte start */
    if ((c & 0xE0) == 0xC0) return true;  /* 2-byte UTF-8 */
    if ((c & 0xF0) == 0xE0) return true;  /* 3-byte UTF-8 */
    if ((c & 0xF8) == 0xF0) return true;  /* 4-byte UTF-8 */

    /* EUC-KR/EUC-JP high byte (0xA1-0xFE) */
    if (c >= 0xA1 && c <= 0xFE) return true;

    /* SHIFT-JIS first byte */
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) return true;

    return false;
}

/**
 * Get expected multibyte sequence length
 * @param c First byte of sequence
 * @return Expected total length (including first byte)
 */
static int l3_get_multibyte_length(unsigned char c)
{
    /* UTF-8 */
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;

    /* EUC-KR/EUC-JP: 2 bytes for hangul/kanji */
    if (c >= 0xA1 && c <= 0xFE) return 2;

    /* SHIFT-JIS: usually 2 bytes */
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) return 2;

    return 1;  /* Single byte */
}

/**
 * Check if multibyte sequence is complete
 * @param buffer Buffer containing bytes
 * @param len Current buffer length
 * @param expected Expected total length
 * @return true if complete, false otherwise
 */
static bool l3_is_multibyte_complete(const unsigned char *buffer, size_t len, int expected)
{
    if (len < (size_t)expected) return false;

    /* Additional validation for UTF-8 continuation bytes */
    unsigned char first = buffer[0];
    if ((first & 0xE0) == 0xC0 || (first & 0xF0) == 0xE0 || (first & 0xF8) == 0xF0) {
        for (size_t i = 1; i < len && i < (size_t)expected; i++) {
            if ((buffer[i] & 0xC0) != 0x80) {
                return false;  /* Invalid UTF-8 continuation */
            }
        }
    }

    return (len >= (size_t)expected);
}

/**
 * Send echo to modem via telnet-to-serial buffer
 * @param l3_ctx Level 3 context
 * @param data Data to echo
 * @param len Length of data
 * @return Number of bytes echoed
 */
static size_t l3_echo_to_modem(l3_context_t *l3_ctx, const unsigned char *data, size_t len)
{
    if (!l3_ctx || !data || len == 0) return 0;

    /* Write to telnet-to-serial buffer for echo */
    size_t written = ts_cbuf_write(&l3_ctx->bridge->ts_telnet_to_serial_buf, data, len);

    if (written > 0) {
        MB_LOG_DEBUG("Echo to modem: %zu bytes", written);

        /* Debug output for echo content */
        if (written <= 10) {
            char hex_str[32];
            size_t pos = 0;
            for (size_t i = 0; i < written && pos < sizeof(hex_str)-3; i++) {
                pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X ", data[i]);
            }
            MB_LOG_DEBUG("Echo bytes: %s", hex_str);
        }
    }

    return written;
}

/* ========== Level 3 Context Management ========== */

/* ========== DCD Event Bridge Functions ========== */

/**
 * Handle DCD rising edge event - activates pipeline when ready
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_on_dcd_rising(l3_context_t *l3_ctx)
{
    printf("[DEBUG-L3] l3_on_dcd_rising() ENTRY: l3_ctx=%p\n", (void*)l3_ctx);
    fflush(stdout);

    if (l3_ctx == NULL) {
        printf("[ERROR-L3] l3_ctx is NULL!\n");
        fflush(stdout);
        return L3_ERROR_INVALID_PARAM;
    }

    printf("[DEBUG-L3] About to try locking state_mutex (non-blocking)\n");
    fflush(stdout);

    /* Use trylock to avoid deadlock with l3_process_state_machine() */
    int lock_ret = pthread_mutex_trylock(&l3_ctx->state_mutex);
    if (lock_ret != 0) {
        /* Mutex is already locked by state machine - just set flags and return */
        printf("[DEBUG-L3] state_mutex is busy (held by state machine), setting flags atomically\n");
        fflush(stdout);

        /* These flag updates are atomic enough for our purposes */
        l3_ctx->dcd_state = true;
        l3_ctx->dcd_rising_detected = true;

        printf("[INFO-L3] DCD rising edge flags set, state machine will process on next cycle\n");
        fflush(stdout);
        MB_LOG_INFO("DCD rising edge detected - flags set for state machine processing");

        return L3_SUCCESS;
    }

    printf("[DEBUG-L3] state_mutex locked successfully\n");
    fflush(stdout);

    printf("[DEBUG-L3] system_state=%d, level1_ready=%d, level2_ready=%d\n",
           l3_ctx->system_state, l3_ctx->level1_ready, l3_ctx->level2_ready);
    fflush(stdout);

    /* Set DCD state */
    l3_ctx->dcd_state = true;
    l3_ctx->dcd_rising_detected = true;

    /* Process DCD rising - state machine will handle L2 connection wait */
    if (l3_ctx->system_state == L3_STATE_READY) {
        printf("[INFO-L3] DCD rising edge detected in READY state - triggering connection\n");
        fflush(stdout);
        MB_LOG_INFO("DCD rising edge detected in READY state - triggering connection");

        /* Signal state machine to process the transition */
        printf("[DEBUG-L3] Broadcasting state_condition\n");
        fflush(stdout);
        pthread_cond_broadcast(&l3_ctx->state_condition);
    } else {
        printf("[DEBUG-L3] DCD rising edge detected in state=%s, L1=%d, L2=%d\n",
               l3_system_state_to_string(l3_ctx->system_state),
               l3_ctx->level1_ready, l3_ctx->level2_ready);
        fflush(stdout);
        MB_LOG_DEBUG("DCD rising edge detected in state: %s (L1=%s, L2=%s)",
                    l3_system_state_to_string(l3_ctx->system_state),
                    l3_ctx->level1_ready ? "Ready" : "Not Ready",
                    l3_ctx->level2_ready ? "Ready" : "Not Ready");
    }

    printf("[DEBUG-L3] About to unlock state_mutex\n");
    fflush(stdout);
    pthread_mutex_unlock(&l3_ctx->state_mutex);
    printf("[DEBUG-L3] l3_on_dcd_rising() returning L3_SUCCESS\n");
    fflush(stdout);
    return L3_SUCCESS;
}

/**
 * Handle DCD falling edge event - triggers graceful shutdown
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_on_dcd_falling(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&l3_ctx->state_mutex);

    l3_ctx->dcd_state = false;
    l3_ctx->dcd_rising_detected = false;

    /* If we're in data transfer mode, initiate graceful shutdown */
    if (l3_ctx->system_state == L3_STATE_DATA_TRANSFER) {
        MB_LOG_INFO("DCD falling edge detected during data transfer - initiating shutdown");
        l3_set_system_state(l3_ctx, L3_STATE_FLUSHING, LEVEL3_SHUTDOWN_TIMEOUT);
    } else if (l3_ctx->system_state == L3_STATE_CONNECTING ||
               l3_ctx->system_state == L3_STATE_NEGOTIATING) {
        MB_LOG_INFO("DCD falling edge detected during connection - aborting to READY");
        l3_set_system_state(l3_ctx, L3_STATE_READY, 0);
    } else {
        MB_LOG_DEBUG("DCD falling edge detected in state: %s",
                    l3_system_state_to_string(l3_ctx->system_state));
    }

    pthread_mutex_unlock(&l3_ctx->state_mutex);
    return L3_SUCCESS;
}

/**
 * Get current DCD state
 * @param l3_ctx Level 3 context
 * @return true if DCD is high, false otherwise
 */
bool l3_get_dcd_state(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return false;
    }

    pthread_mutex_lock(&l3_ctx->state_mutex);
    bool dcd_state = l3_ctx->dcd_state;
    pthread_mutex_unlock(&l3_ctx->state_mutex);

    return dcd_state;
}

/**
 * Initialize DCD monitoring for Level 3
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_init_dcd_monitoring(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&l3_ctx->state_mutex);

    /* Initialize DCD state */
    l3_ctx->dcd_state = false;
    l3_ctx->dcd_rising_detected = false;
    l3_ctx->dcd_change_time = time(NULL);

    MB_LOG_INFO("DCD monitoring initialized for Level 3");

    pthread_mutex_unlock(&l3_ctx->state_mutex);
    return L3_SUCCESS;
}

/* ========== Enhanced State Machine Functions ========== */

const char *l3_system_state_to_string(l3_system_state_t state)
{
    switch (state) {
        case L3_STATE_UNINITIALIZED:  return "UNINITIALIZED";
        case L3_STATE_INITIALIZING:   return "INITIALIZING";
        case L3_STATE_READY:          return "READY";
        case L3_STATE_CONNECTING:     return "CONNECTING";
        case L3_STATE_NEGOTIATING:    return "NEGOTIATING";
        case L3_STATE_DATA_TRANSFER:  return "DATA_TRANSFER";
        case L3_STATE_FLUSHING:       return "FLUSHING";
        case L3_STATE_SHUTTING_DOWN:  return "SHUTTING_DOWN";
        case L3_STATE_TERMINATED:     return "TERMINATED";
        case L3_STATE_ERROR:          return "ERROR";
        default:                      return "UNKNOWN";
    }
}

int l3_set_system_state(l3_context_t *l3_ctx, l3_system_state_t new_state, int timeout_seconds)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&l3_ctx->state_mutex);

    /* Check if transition is valid */
    if (!l3_is_valid_state_transition(l3_ctx->system_state, new_state)) {
        MB_LOG_ERROR("Invalid state transition: %s -> %s",
                    l3_system_state_to_string(l3_ctx->system_state),
                    l3_system_state_to_string(new_state));
        pthread_mutex_unlock(&l3_ctx->state_mutex);
        return L3_ERROR_INVALID_STATE;
    }

    /* Update state */
    l3_system_state_t old_state = l3_ctx->system_state;
    l3_ctx->previous_state = old_state;
    l3_ctx->system_state = new_state;
    l3_ctx->state_change_time = time(NULL);
    l3_ctx->state_timeout = timeout_seconds;
    l3_ctx->state_transitions++;

    MB_LOG_INFO("Level 3 state transition: %s -> %s (timeout: %ds)",
                l3_system_state_to_string(old_state),
                l3_system_state_to_string(new_state),
                timeout_seconds);

    /* Special handling for DATA_TRANSFER state */
    if (new_state == L3_STATE_DATA_TRANSFER) {
        /* Set Hayes filter to online mode when entering data transfer */
        l3_ctx->pipeline_serial_to_telnet.filter_state.hayes_ctx.in_online_mode = true;
        MB_LOG_INFO("Hayes filter set to ONLINE mode for data transfer");
    } else if (old_state == L3_STATE_DATA_TRANSFER &&
               (new_state == L3_STATE_FLUSHING || new_state == L3_STATE_SHUTTING_DOWN)) {
        /* Set Hayes filter back to command mode when leaving data transfer */
        l3_ctx->pipeline_serial_to_telnet.filter_state.hayes_ctx.in_online_mode = false;
        MB_LOG_INFO("Hayes filter set to COMMAND mode");
    }

    /* Signal state change */
    pthread_cond_broadcast(&l3_ctx->state_condition);

    pthread_mutex_unlock(&l3_ctx->state_mutex);
    return L3_SUCCESS;
}

bool l3_is_valid_state_transition(l3_system_state_t from_state, l3_system_state_t to_state)
{
    /* Same state - no transition */
    if (from_state == to_state) {
        return false;
    }

    switch (from_state) {
        case L3_STATE_UNINITIALIZED:
            return (to_state == L3_STATE_INITIALIZING);

        case L3_STATE_INITIALIZING:
            return (to_state == L3_STATE_READY || to_state == L3_STATE_ERROR);

        case L3_STATE_READY:
            return (to_state == L3_STATE_CONNECTING || to_state == L3_STATE_SHUTTING_DOWN || to_state == L3_STATE_ERROR);

        case L3_STATE_CONNECTING:
            return (to_state == L3_STATE_NEGOTIATING || to_state == L3_STATE_DATA_TRANSFER || to_state == L3_STATE_READY || to_state == L3_STATE_ERROR);

        case L3_STATE_NEGOTIATING:
            return (to_state == L3_STATE_DATA_TRANSFER || to_state == L3_STATE_CONNECTING || to_state == L3_STATE_ERROR);

        case L3_STATE_DATA_TRANSFER:
            return (to_state == L3_STATE_FLUSHING || to_state == L3_STATE_SHUTTING_DOWN || to_state == L3_STATE_ERROR);

        case L3_STATE_FLUSHING:
            return (to_state == L3_STATE_TERMINATED || to_state == L3_STATE_SHUTTING_DOWN || to_state == L3_STATE_ERROR);

        case L3_STATE_SHUTTING_DOWN:
            return (to_state == L3_STATE_TERMINATED || to_state == L3_STATE_ERROR);

        case L3_STATE_TERMINATED:
            /* Terminal state - no further transitions */
            return false;

        case L3_STATE_ERROR:
            /* From error, can attempt recovery or shutdown */
            return (to_state == L3_STATE_READY || to_state == L3_STATE_SHUTTING_DOWN || to_state == L3_STATE_TERMINATED);

        default:
            return false;
    }
}

int l3_process_state_machine(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&l3_ctx->state_mutex);

    int ret = L3_SUCCESS;
    l3_system_state_t current_state = l3_ctx->system_state;

    /* Check for timeout */
    if (l3_is_state_timed_out(l3_ctx)) {
        ret = l3_handle_state_timeout(l3_ctx);
        if (ret != L3_SUCCESS) {
            MB_LOG_ERROR("State timeout handling failed");
            pthread_mutex_unlock(&l3_ctx->state_mutex);
            return ret;
        }
    }

    /* Process current state */
    switch (current_state) {
        case L3_STATE_INITIALIZING:
            /* Check if initialization is complete */
            l3_ctx->level1_ready = l3_ctx->bridge->serial_ready && l3_ctx->bridge->modem_ready;
#ifdef ENABLE_LEVEL2
            l3_ctx->level2_ready = telnet_is_connected(&l3_ctx->bridge->telnet);
#else
            l3_ctx->level2_ready = false;
#endif

            printf("[DEBUG-STATE-MACHINE] INITIALIZING: L1_ready=%d, L2_ready=%d, DCD_detected=%d\n",
                   l3_ctx->level1_ready, l3_ctx->level2_ready, l3_ctx->dcd_rising_detected);
            fflush(stdout);

            /* Level 3 strategy: L1 ready is sufficient to enter READY state
             * L2 (telnet) will be connected when DCD rising edge occurs */
            if (l3_ctx->level1_ready) {
                /* Level 1 ready - move to READY state (L2 will connect on DCD rising) */
                printf("[INFO-STATE-MACHINE] Level 1 ready - transitioning to READY (L2 will connect on DCD)\n");
                fflush(stdout);
                MB_LOG_INFO("Level 1 ready - entering READY state (L2=%s)",
                           l3_ctx->level2_ready ? "connected" : "will connect on DCD");
                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_READY, 0);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            } else {
                /* Level 1 not ready - wait */
                static int log_counter_l1 = 0;
                if (++log_counter_l1 % 10 == 1) {  /* Log every 10 cycles */
                    printf("[DEBUG-STATE-MACHINE] Waiting for Level 1 (serial/modem) connection\n");
                    fflush(stdout);
                    MB_LOG_DEBUG("Waiting for Level 1 (serial/modem) connection");
                }
            }
            break;

        case L3_STATE_READY:
            /* Wait for DCD rising edge to trigger connection */
            /* Update L2 status in case it connects while waiting */
            l3_ctx->level2_ready = telnet_is_connected(&l3_ctx->bridge->telnet);

            /* Reduce log spam - only log periodically (every 5 seconds) */
            static int log_counter_ready = 0;
            if (++log_counter_ready % 10 == 1) {
                printf("[DEBUG-STATE-MACHINE] READY: DCD_rising_detected=%d, L2_ready=%d\n",
                       l3_ctx->dcd_rising_detected, l3_ctx->level2_ready);
                fflush(stdout);
            }

            if (l3_ctx->dcd_rising_detected) {
                printf("[INFO-STATE-MACHINE] DCD rising edge detected in READY - starting connection (L2=%s)\n",
                       l3_ctx->level2_ready ? "connected" : "not connected yet");
                fflush(stdout);
                MB_LOG_INFO("DCD rising edge detected - starting connection (L2=%s)",
                           l3_ctx->level2_ready ? "connected" : "will wait for connection");
                l3_ctx->dcd_rising_detected = false;  /* Reset flag */

                /* Reset connection attempt flag for CONNECTING state */
                extern bool g_level3_connection_attempted;
                g_level3_connection_attempted = false;

                /* Reset transition log flag for next connection */
                extern bool g_level3_transition_logged;
                g_level3_transition_logged = false;

                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_CONNECTING, LEVEL3_CONNECT_TIMEOUT);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            }
            break;

        case L3_STATE_CONNECTING:
            /* Attempt telnet connection if not already connected */
            if (!telnet_is_connected(&l3_ctx->bridge->telnet)) {
                /* Check if we should attempt connection (first time or after error) */
                time_t now = time(NULL);

                /* Attempt connection: first time entering CONNECTING, or retry after 2 seconds */
                if (!g_level3_connection_attempted || (now - g_level3_last_attempt) >= 2) {
                    printf("[INFO-STATE-MACHINE] Attempting telnet connection to %s:%d\n",
                           l3_ctx->bridge->config->telnet_host, l3_ctx->bridge->config->telnet_port);
                    fflush(stdout);
                    MB_LOG_INFO("Attempting telnet connection to %s:%d",
                               l3_ctx->bridge->config->telnet_host, l3_ctx->bridge->config->telnet_port);

                    /* Temporarily unlock mutex for telnet_connect() */
                    pthread_mutex_unlock(&l3_ctx->state_mutex);
                    int connect_result = telnet_connect(&l3_ctx->bridge->telnet,
                                                        l3_ctx->bridge->config->telnet_host,
                                                        l3_ctx->bridge->config->telnet_port);
                    pthread_mutex_lock(&l3_ctx->state_mutex);

                    g_level3_connection_attempted = true;
                    g_level3_last_attempt = now;

                    if (connect_result == SUCCESS) {
                        printf("[INFO-STATE-MACHINE] Telnet connection successful\n");
                        fflush(stdout);
                        MB_LOG_INFO("Telnet connection established successfully");
                    } else {
                        printf("[ERROR-STATE-MACHINE] Telnet connection failed: %d\n", connect_result);
                        fflush(stdout);
                        MB_LOG_ERROR("Telnet connection failed: %d (will retry)", connect_result);
                    }
                }
            }

            /* Check if Level 2 (telnet) is connected */
            l3_ctx->level2_ready = telnet_is_connected(&l3_ctx->bridge->telnet);

            if (l3_ctx->level2_ready) {
                /* Level 2 connected - proceed to data transfer */
                if (!g_level3_transition_logged) {
                    time_t now = time(NULL);
                    struct tm *tm_info = localtime(&now);
                    char timestamp[32];
                    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

                    printf("[%s][INFO-STATE-MACHINE] Level 2 connected - proceeding to data transfer\n", timestamp);
                    fflush(stdout);
                    MB_LOG_INFO("Level 2 telnet connected - entering data transfer mode");
                    g_level3_transition_logged = true;
                }

                l3_ctx->negotiation_complete = true;  /* Mark as complete */
                l3_ctx->level3_active = true;  /* Enable Level 3 processing BEFORE state transition */
                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_DATA_TRANSFER, 0);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            } else {
                /* Still waiting for Level 2 connection */
                static int log_counter_connecting = 0;
                if (++log_counter_connecting % 5 == 1) {  /* Log every 5 cycles */
                    time_t now = time(NULL);
                    struct tm *tm_info = localtime(&now);
                    char timestamp[32];
                    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

                    printf("[%s][DEBUG-STATE-MACHINE] Waiting for Level 2 telnet connection...\n", timestamp);
                    fflush(stdout);
                    MB_LOG_DEBUG("Waiting for Level 2 telnet connection");
                }
            }
            break;

        case L3_STATE_NEGOTIATING:
            /* Check if negotiation is complete */
            if (l3_ctx->negotiation_complete) {
                MB_LOG_INFO("Protocol negotiation complete - entering data transfer mode");
                l3_ctx->level3_active = true;  /* Enable Level 3 processing BEFORE state transition */
                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_DATA_TRANSFER, 0);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            }
            break;

        case L3_STATE_DATA_TRANSFER:
            /* Active data processing - handled by main thread */
            /* Check for DCD falling edge to trigger shutdown */
            /* IMPORTANT: Check dcd_state (persistent), not dcd_rising_detected (one-shot event) */
            if (!l3_ctx->dcd_state) {
                MB_LOG_INFO("DCD lost - initiating graceful shutdown");
                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_FLUSHING, LEVEL3_SHUTDOWN_TIMEOUT);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            }
            break;

        case L3_STATE_FLUSHING:
            /* Check if buffers are empty and ready to shutdown */
            bool serial_empty = (l3_double_buffer_available(&l3_ctx->pipeline_serial_to_telnet.buffers) == 0);
            bool telnet_empty = (l3_double_buffer_available(&l3_ctx->pipeline_telnet_to_serial.buffers) == 0);

            if (serial_empty && telnet_empty) {
                MB_LOG_INFO("Buffers flushed - shutting down");
                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_SHUTTING_DOWN, 5);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            }
            break;

        case L3_STATE_SHUTTING_DOWN:
            /* Graceful shutdown - disable Level 3 processing */
            l3_ctx->level3_active = false;
            pthread_mutex_unlock(&l3_ctx->state_mutex);
            ret = l3_set_system_state(l3_ctx, L3_STATE_TERMINATED, 0);
            pthread_mutex_lock(&l3_ctx->state_mutex);
            break;

        case L3_STATE_TERMINATED:
            /* Terminal state - wait for shutdown request */
            if (l3_ctx->shutdown_requested) {
                MB_LOG_INFO("Shutdown request received - terminating thread");
                l3_ctx->thread_running = false;
            }
            break;

        case L3_STATE_ERROR:
            /* Error recovery - attempt to return to READY state */
            MB_LOG_WARNING("In error state - attempting recovery");
            pthread_mutex_unlock(&l3_ctx->state_mutex);
            ret = l3_set_system_state(l3_ctx, L3_STATE_READY, 0);
            pthread_mutex_lock(&l3_ctx->state_mutex);
            break;

        default:
            MB_LOG_ERROR("Unknown state: %d", current_state);
            pthread_mutex_unlock(&l3_ctx->state_mutex);
            ret = l3_set_system_state(l3_ctx, L3_STATE_ERROR, 0);
            pthread_mutex_lock(&l3_ctx->state_mutex);
            break;
    }

    pthread_mutex_unlock(&l3_ctx->state_mutex);
    return ret;
}

int l3_handle_state_timeout(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    l3_system_state_t current_state = l3_ctx->system_state;
    time_t now = time(NULL);
    time_t time_in_state = now - l3_ctx->state_change_time;

    MB_LOG_WARNING("State timeout detected: %s (in state for %ld seconds, timeout: %ds)",
                l3_system_state_to_string(current_state),
                (long)time_in_state,
                l3_ctx->state_timeout);

    switch (current_state) {
        case L3_STATE_INITIALIZING:
            /* Initialization timeout - try to start anyway */
            MB_LOG_WARNING("Initialization timeout - proceeding with partial initialization");
            return l3_set_system_state(l3_ctx, L3_STATE_READY, 0);

        case L3_STATE_CONNECTING:
            /* Connection timeout - return to ready state */
            MB_LOG_WARNING("Connection timeout - returning to ready state");
            return l3_set_system_state(l3_ctx, L3_STATE_READY, 0);

        case L3_STATE_NEGOTIATING:
            /* Negotiation timeout - proceed anyway */
            MB_LOG_WARNING("Negotiation timeout - proceeding with default settings");
            l3_ctx->negotiation_complete = true;
            return l3_set_system_state(l3_ctx, L3_STATE_DATA_TRANSFER, 0);

        case L3_STATE_FLUSHING:
            /* Flush timeout - force shutdown */
            MB_LOG_WARNING("Flush timeout - forcing shutdown");
            return l3_set_system_state(l3_ctx, L3_STATE_SHUTTING_DOWN, 5);

        case L3_STATE_SHUTTING_DOWN:
            /* Shutdown timeout - force termination */
            MB_LOG_ERROR("Shutdown timeout - forcing termination");
            return l3_set_system_state(l3_ctx, L3_STATE_TERMINATED, 0);

        default:
            /* Other states - transition to error */
            MB_LOG_ERROR("Unhandled timeout in state %s", l3_system_state_to_string(current_state));
            return l3_set_system_state(l3_ctx, L3_STATE_ERROR, 0);
    }
}

bool l3_is_state_timed_out(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL || l3_ctx->state_timeout <= 0) {
        return false;
    }

    time_t now = time(NULL);
    time_t time_in_state = now - l3_ctx->state_change_time;

    return (time_in_state >= l3_ctx->state_timeout);
}

/* ========== Level 3 Context Management ========== */

int l3_init(l3_context_t *l3_ctx, bridge_ctx_t *bridge_ctx)
{
    if (l3_ctx == NULL || bridge_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(l3_ctx, 0, sizeof(l3_context_t));
    l3_ctx->bridge = bridge_ctx;

    /* Initialize state machine */
    l3_ctx->system_state = L3_STATE_UNINITIALIZED;
    l3_ctx->previous_state = L3_STATE_UNINITIALIZED;
    l3_ctx->state_change_time = time(NULL);
    l3_ctx->state_timeout = 0;
    l3_ctx->state_transitions = 0;

    /* Initialize state machine synchronization */
    if (pthread_mutex_init(&l3_ctx->state_mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize state mutex");
        return L3_ERROR_THREAD;
    }
    if (pthread_cond_init(&l3_ctx->state_condition, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize state condition");
        pthread_mutex_destroy(&l3_ctx->state_mutex);
        return L3_ERROR_THREAD;
    }

    /* Set initial state */
    int ret = l3_set_system_state(l3_ctx, L3_STATE_INITIALIZING, LEVEL3_INIT_TIMEOUT);
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to set initial state");
        pthread_mutex_destroy(&l3_ctx->state_mutex);
        pthread_cond_destroy(&l3_ctx->state_condition);
        return ret;
    }

    /* Initialize pipelines */
    ret = l3_pipeline_init(&l3_ctx->pipeline_serial_to_telnet,
                           L3_PIPELINE_SERIAL_TO_TELNET,
                           "Serial→Telnet");
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to initialize Serial→Telnet pipeline");
        return ret;
    }

    ret = l3_pipeline_init(&l3_ctx->pipeline_telnet_to_serial,
                          L3_PIPELINE_TELNET_TO_SERIAL,
                          "Telnet→Serial");
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to initialize Telnet→Serial pipeline");
        return ret;
    }

    /* Initialize scheduling */
    pthread_mutex_init(&l3_ctx->scheduling_mutex, NULL);
    l3_ctx->scheduling_start_time = time(NULL);
    l3_ctx->round_robin_counter = 0;

    /* Initialize enhanced scheduling system */
    ret = l3_init_enhanced_scheduling(l3_ctx);
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to initialize enhanced scheduling system");
        /* Continue with basic scheduling - not fatal */
    }

    /* Initialize half-duplex control */
    l3_ctx->active_pipeline = L3_PIPELINE_SERIAL_TO_TELNET;  /* Start with pipeline 1 */
    l3_ctx->half_duplex_mode = true;  /* Enable half-duplex by default */
    l3_ctx->last_pipeline_switch = time(NULL);

    /* Initialize system state */
    l3_ctx->level3_active = false;
    l3_ctx->level1_ready = false;
    l3_ctx->level2_ready = false;

    /* Initialize performance monitoring */
    l3_ctx->total_pipeline_switches = 0;
    l3_ctx->system_utilization_pct = 0.0;
    l3_ctx->start_time = time(NULL);

    /* Thread control */
    l3_ctx->thread_running = false;

    MB_LOG_INFO("Level 3 context initialized successfully");
    return L3_SUCCESS;
}

int l3_start(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    MB_LOG_INFO("Starting Level 3 pipeline management");

    /* Check if both Level 1 and Level 2 are ready */
    l3_ctx->level1_ready = l3_ctx->bridge->serial_ready && l3_ctx->bridge->modem_ready;

#ifdef ENABLE_LEVEL2
    l3_ctx->level2_ready = telnet_is_connected(&l3_ctx->bridge->telnet);
#else
    l3_ctx->level2_ready = false;
    MB_LOG_WARNING("Level 3: Level 2 (telnet) not available in this build");
#endif

    if (!l3_ctx->level1_ready) {
        MB_LOG_WARNING("Level 3: Level 1 (serial/modem) not ready - will wait");
    }

    if (!l3_ctx->level2_ready) {
        MB_LOG_WARNING("Level 3: Level 2 (telnet) not ready - will wait");
    }

    /* Start Level 3 management thread */
    l3_ctx->thread_running = true;
    l3_ctx->level3_active = true;

    int ret = pthread_create(&l3_ctx->level3_thread, NULL, l3_management_thread_func, l3_ctx);
    if (ret != 0) {
        MB_LOG_ERROR("Failed to create Level 3 management thread: %s", strerror(ret));
        l3_ctx->thread_running = false;
        l3_ctx->level3_active = false;
        return L3_ERROR_THREAD;
    }

    MB_LOG_INFO("Level 3 management thread started successfully");
    return L3_SUCCESS;
}

int l3_stop(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    MB_LOG_INFO("Stopping Level 3 pipeline management");

    /* Signal thread to stop */
    l3_ctx->thread_running = false;
    l3_ctx->level3_active = false;

    /* Wait for thread to exit */
    if (pthread_join(l3_ctx->level3_thread, NULL) != 0) {
        MB_LOG_WARNING("Failed to join Level 3 management thread");
    }

    /* Print final statistics */
    l3_print_stats(l3_ctx);

    MB_LOG_INFO("Level 3 pipeline management stopped");
    return L3_SUCCESS;
}

void l3_cleanup(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return;
    }

    /* Cleanup pipelines */
    pthread_mutex_destroy(&l3_ctx->pipeline_serial_to_telnet.buffers.mutex);
    pthread_mutex_destroy(&l3_ctx->pipeline_telnet_to_serial.buffers.mutex);

    /* Cleanup scheduling */
    pthread_mutex_destroy(&l3_ctx->scheduling_mutex);

    memset(l3_ctx, 0, sizeof(l3_context_t));
    MB_LOG_INFO("Level 3 context cleaned up");
}

/* ========== Pipeline Management ========== */

int l3_pipeline_init(l3_pipeline_t *pipeline, l3_pipeline_direction_t direction, const char *name)
{
    if (pipeline == NULL || name == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(pipeline, 0, sizeof(l3_pipeline_t));
    pipeline->direction = direction;
    strncpy(pipeline->name, name, sizeof(pipeline->name) - 1);
    pipeline->name[sizeof(pipeline->name) - 1] = '\0';

    /* Initialize double buffer */
    int ret = l3_double_buffer_init(&pipeline->buffers);
    if (ret != L3_SUCCESS) {
        return ret;
    }

    /* Initialize protocol filter state */
    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        /* Initialize Hayes filter context with dictionary */
        memset(&pipeline->filter_state.hayes_ctx, 0, sizeof(hayes_filter_context_t));
        pipeline->filter_state.hayes_ctx.state = HAYES_STATE_NORMAL;
        pipeline->filter_state.hayes_ctx.dict = &hayes_dictionary;
        pipeline->filter_state.hayes_ctx.in_online_mode = false;  /* Start in command mode */
        pipeline->filter_state.hayes_ctx.last_char_time = l3_get_timestamp_ms();
    } else {
        pipeline->filter_state.telnet_state = TELNET_FILTER_STATE_DATA;
    }

    /* Initialize fair scheduling */
    pipeline->last_timeslice_start = time(NULL);
    pipeline->timeslice_duration_ms = L3_FAIRNESS_TIME_SLICE_MS;
    pipeline->bytes_in_timeslice = 0;

    /* Initialize backpressure */
    pipeline->backpressure_active = false;
    pipeline->backpressure_start = 0;

    /* Initialize statistics */
    pipeline->total_bytes_processed = 0;
    pipeline->total_bytes_dropped = 0;
    pipeline->pipeline_switches = 0;
    pipeline->avg_processing_time_ms = 0.0;

    MB_LOG_DEBUG("Pipeline initialized: %s", pipeline->name);
    return L3_SUCCESS;
}

int l3_pipeline_process(l3_pipeline_t *pipeline, const unsigned char *input_data, size_t input_len,
                       unsigned char *output_data, size_t output_size, size_t *output_len)
{
    if (pipeline == NULL || input_data == NULL || output_data == NULL || output_len == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (input_len == 0) {
        *output_len = 0;
        return L3_SUCCESS;
    }

    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    size_t filtered_len = 0;
    int ret = L3_SUCCESS;

    /* Apply protocol-specific filtering */
    if (pipeline->direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        /* Pipeline 1: Filter Hayes commands with enhanced dictionary support */
        ret = l3_filter_hayes_commands(&pipeline->filter_state.hayes_ctx,
                                       input_data, input_len,
                                       output_data, output_size, &filtered_len);
    } else {
        /* Pipeline 2: Filter TELNET control codes */
        ret = l3_filter_telnet_controls(&pipeline->filter_state.telnet_state,
                                        input_data, input_len,
                                        output_data, output_size, &filtered_len);
    }

    if (ret != L3_SUCCESS) {
        MB_LOG_WARNING("Pipeline %s: Filtering failed", pipeline->name);
        *output_len = 0;
        return ret;
    }

    /* Update pipeline statistics */
    gettimeofday(&end_time, NULL);
    double processing_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                            (end_time.tv_usec - start_time.tv_usec) / 1000.0;

    l3_update_pipeline_stats(pipeline, filtered_len, processing_time);

    *output_len = filtered_len;
    return L3_SUCCESS;
}

int l3_pipeline_switch_buffers(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&pipeline->buffers.mutex);

    /* Switch active buffers */
    pipeline->buffers.main_active = !pipeline->buffers.main_active;

    /* Reset the new main buffer's read position */
    if (!pipeline->buffers.main_active) {
        pipeline->buffers.main_pos = 0;
    } else {
        pipeline->buffers.main_pos = 0;
    }

    /* Clear the new sub buffer */
    if (pipeline->buffers.main_active) {
        pipeline->buffers.sub_len = 0;
    } else {
        pipeline->buffers.sub_len = 0;
    }

    pipeline->pipeline_switches++;

    pthread_mutex_unlock(&pipeline->buffers.mutex);

    MB_LOG_DEBUG("Pipeline %s: Switched buffers (switch #%llu)",
                pipeline->name, (unsigned long long)pipeline->pipeline_switches);
    return L3_SUCCESS;
}

/* ========== Double Buffer Management ========== */

int l3_double_buffer_init(l3_double_buffer_t *dbuf)
{
    if (dbuf == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(dbuf, 0, sizeof(l3_double_buffer_t));

    /* Initialize buffer state */
    dbuf->main_active = true;
    dbuf->main_len = 0;
    dbuf->main_pos = 0;
    dbuf->sub_len = 0;

    /* Initialize synchronization */
    int ret = pthread_mutex_init(&dbuf->mutex, NULL);
    if (ret != 0) {
        MB_LOG_ERROR("Failed to initialize double buffer mutex: %s", strerror(ret));
        return L3_ERROR_THREAD;
    }

    return L3_SUCCESS;
}

size_t l3_double_buffer_write(l3_double_buffer_t *dbuf, const unsigned char *data, size_t len)
{
    if (dbuf == NULL || data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);

    /* Check if sub-buffer has space */
    size_t available_space = L3_PIPELINE_BUFFER_SIZE - dbuf->sub_len;
    size_t to_write = len < available_space ? len : available_space;

    if (to_write > 0) {
        memcpy(dbuf->sub_data + dbuf->sub_len, data, to_write);
        dbuf->sub_len += to_write;
        dbuf->last_activity = time(NULL);
    } else {
        /* Buffer overflow - drop data */
        dbuf->bytes_dropped += len;
        MB_LOG_WARNING("Double buffer overflow: dropped %zu bytes", len);
    }

    pthread_mutex_unlock(&dbuf->mutex);
    return to_write;
}

size_t l3_double_buffer_read(l3_double_buffer_t *dbuf, unsigned char *data, size_t len)
{
    if (dbuf == NULL || data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);

    /* Read from main buffer */
    size_t available = dbuf->main_len - dbuf->main_pos;
    size_t to_read = len < available ? len : available;

    if (to_read > 0) {
        memcpy(data, dbuf->main_data + dbuf->main_pos, to_read);
        dbuf->main_pos += to_read;
        dbuf->bytes_processed += to_read;
        dbuf->last_activity = time(NULL);
    }

    pthread_mutex_unlock(&dbuf->mutex);
    return to_read;
}

size_t l3_double_buffer_available(l3_double_buffer_t *dbuf)
{
    if (dbuf == NULL) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);
    size_t available = dbuf->main_len - dbuf->main_pos;
    pthread_mutex_unlock(&dbuf->mutex);

    return available;
}

size_t l3_double_buffer_free(l3_double_buffer_t *dbuf)
{
    if (dbuf == NULL) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);
    size_t free_space = L3_PIPELINE_BUFFER_SIZE - dbuf->sub_len;
    pthread_mutex_unlock(&dbuf->mutex);

    return free_space;
}

/* ========== Protocol Filtering ========== */

/* Hayes Command Dictionary - Based on modem.c implementation */
static const hayes_command_entry_t hayes_commands[] = {
    /* Basic AT Commands */
    {"ATA",  HAYES_CMD_BASIC, false, 0, 0, "Answer incoming call"},
    {"ATB",  HAYES_CMD_BASIC, true,  0, 1, "Bell/CCITT mode"},
    {"ATD",  HAYES_CMD_BASIC, true,  0, 0, "Dial number"},
    {"ATE",  HAYES_CMD_BASIC, true,  0, 1, "Echo on/off"},
    {"ATH",  HAYES_CMD_BASIC, true,  0, 1, "Hang up"},
    {"ATI",  HAYES_CMD_BASIC, true,  0, 9, "Information/identification"},
    {"ATL",  HAYES_CMD_BASIC, true,  0, 3, "Speaker volume"},
    {"ATM",  HAYES_CMD_BASIC, true,  0, 3, "Speaker control"},
    {"ATO",  HAYES_CMD_BASIC, true,  0, 0, "Return to online mode"},
    {"ATQ",  HAYES_CMD_BASIC, true,  0, 1, "Quiet mode"},
    {"ATS",  HAYES_CMD_REGISTER, true, 0, 255, "S-register access"},
    {"ATV",  HAYES_CMD_BASIC, true,  0, 1, "Verbose mode"},
    {"ATX",  HAYES_CMD_BASIC, true,  0, 4, "Extended result codes"},
    {"ATZ",  HAYES_CMD_BASIC, true,  0, 1, "Reset modem"},

    /* Extended AT& Commands */
    {"AT&C", HAYES_CMD_EXTENDED, true, 0, 1, "DCD control"},
    {"AT&D", HAYES_CMD_EXTENDED, true, 0, 3, "DTR control"},
    {"AT&F", HAYES_CMD_EXTENDED, false, 0, 0, "Factory defaults"},
    {"AT&V", HAYES_CMD_EXTENDED, false, 0, 0, "View configuration"},
    {"AT&W", HAYES_CMD_EXTENDED, true, 0, 1, "Write configuration"},
    {"AT&S", HAYES_CMD_EXTENDED, true, 0, 1, "DSR override"},

    /* Escape sequence */
    {"+++",  HAYES_CMD_BASIC, false, 0, 0, "Escape to command mode"}
};

/* Hayes Result Codes */
static const hayes_result_entry_t hayes_results[] = {
    {"OK",           false, false},
    {"ERROR",        false, false},
    {"CONNECT",      true,  true},
    {"NO CARRIER",   true,  false},
    {"NO DIALTONE",  true,  false},
    {"BUSY",         true,  false},
    {"NO ANSWER",    true,  false},
    {"RING",         false, false},
    {"DELAYED",      false, false},
    {"BLACKLISTED",  false, false}
};

/* Global Hayes Dictionary */
static const hayes_dictionary_t hayes_dictionary = {
    .commands = hayes_commands,
    .num_commands = sizeof(hayes_commands) / sizeof(hayes_commands[0]),
    .results = hayes_results,
    .num_results = sizeof(hayes_results) / sizeof(hayes_results[0])
};

/**
 * Check if buffer contains a Hayes command
 */
static bool l3_is_hayes_command(const unsigned char *buffer, size_t len, const hayes_dictionary_t *dict)
{
    if (len < 2 || dict == NULL) return false;

    /* Check for "AT" prefix (case insensitive) */
    if ((buffer[0] == 'A' || buffer[0] == 'a') &&
        (buffer[1] == 'T' || buffer[1] == 't')) {

        /* For each known command */
        for (size_t i = 0; i < dict->num_commands; i++) {
            const char *cmd = dict->commands[i].command;
            size_t cmd_len = strlen(cmd);

            /* Check if buffer starts with this command (case insensitive) */
            if (len >= cmd_len) {
                bool match = true;
                for (size_t j = 0; j < cmd_len; j++) {
                    char b = buffer[j];
                    char c = cmd[j];
                    /* Case insensitive comparison */
                    if (b >= 'a' && b <= 'z') b -= 32;
                    if (c >= 'a' && c <= 'z') c -= 32;
                    if (b != c) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    MB_LOG_DEBUG("Detected Hayes command: %s", cmd);
                    return true;
                }
            }
        }
    }

    /* Check for +++ escape sequence */
    if (len >= 3 && buffer[0] == '+' && buffer[1] == '+' && buffer[2] == '+') {
        MB_LOG_DEBUG("Detected Hayes escape sequence: +++");
        return true;
    }

    return false;
}

/**
 * Check if buffer contains a Hayes result code
 */
static bool l3_is_hayes_result(const unsigned char *buffer, size_t len, const hayes_dictionary_t *dict)
{
    if (len < 2 || dict == NULL) return false;

    /* For each known result code */
    for (size_t i = 0; i < dict->num_results; i++) {
        const char *result = dict->results[i].code;
        size_t result_len = strlen(result);

        /* Check if buffer contains this result code */
        if (len >= result_len) {
            if (memcmp(buffer, result, result_len) == 0) {
                MB_LOG_DEBUG("Detected Hayes result: %s", result);
                return true;
            }
        }
    }

    return false;
}

/**
 * Enhanced Hayes filter with dictionary support and line buffering
 */
int l3_filter_hayes_commands(hayes_filter_context_t *ctx, const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_size, size_t *output_len)
{
    if (ctx == NULL || input == NULL || output == NULL || output_len == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Initialize dictionary if not set */
    if (ctx->dict == NULL) {
        ctx->dict = &hayes_dictionary;
    }

    size_t out_pos = 0;
    *output_len = 0;
    long long current_time = l3_get_timestamp_ms();

    /* Process input data */
    for (size_t i = 0; i < input_len && out_pos < output_size; i++) {
        unsigned char c = input[i];

        /* ONLINE MODE: Line buffering with AT command filtering */
        if (ctx->in_online_mode) {
            /* Special case: +++ escape sequence detection */
            if (c == '+') {
                /* Check for +++ escape sequence with guard time */
                if (ctx->plus_count == 0) {
                    long long elapsed = current_time - ctx->last_char_time;
                    if (elapsed >= 1000) { /* 1 second guard time before first + */
                        ctx->plus_start_time = current_time;
                        ctx->plus_count = 1;
                        ctx->last_char_time = current_time;
                        continue; /* Don't output yet */
                    }
                } else if (ctx->plus_count == 1 || ctx->plus_count == 2) {
                    ctx->plus_count++;
                    if (ctx->plus_count == 3) {
                        /* +++ detected - switch to command mode */
                        MB_LOG_INFO("Hayes filter: +++ escape detected, switching to COMMAND mode");
                        ctx->in_online_mode = false;
                        ctx->state = HAYES_STATE_NORMAL;
                        ctx->plus_count = 0;
                        ctx->line_len = 0;
                        continue; /* Don't output the +++ */
                    }
                    ctx->last_char_time = current_time;
                    continue; /* Don't output yet */
                }
            } else if (ctx->plus_count > 0) {
                /* Not a + - flush buffered + characters */
                for (int j = 0; j < ctx->plus_count; j++) {
                    if (out_pos < output_size) output[out_pos++] = '+';
                }
                ctx->plus_count = 0;
            }

            /* Line buffering for AT command detection */
            /* Track start time for new line */
            if (ctx->line_len == 0) {
                ctx->line_start_time = current_time;
            }

            /* Add to line buffer */
            if (ctx->line_len < sizeof(ctx->line_buffer) - 1) {
                ctx->line_buffer[ctx->line_len++] = c;
                ctx->line_buffer[ctx->line_len] = '\0';
            }

            /* Check if we have a complete line */
            if (c == '\r' || c == '\n') {
                /* Process complete line */
                bool is_at_command = false;

                /* Check for AT command (minimum 3 chars: "AT\r") */
                if (ctx->line_len >= 3) {
                    /* Check if line starts with AT (all 4 combinations: AT, At, aT, at) */
                    if ((ctx->line_buffer[0] == 'A' || ctx->line_buffer[0] == 'a') &&
                        (ctx->line_buffer[1] == 'T' || ctx->line_buffer[1] == 't')) {
                        /* Check what comes after AT - must be a command character or line ending */
                        if (ctx->line_len == 3) {
                            /* Just "AT\r" or "AT\n" */
                            is_at_command = true;
                        } else {
                            unsigned char next_char = ctx->line_buffer[2];
                            /* AT commands can be followed by:
                             * - Uppercase letter (ATH, ATZ, ATE, etc.)
                             * - Digit (AT0, AT1, etc.)
                             * - Special chars (+, &, %, etc. for extended commands)
                             * - CR/LF
                             */
                            if ((next_char >= 'A' && next_char <= 'Z') ||
                                (next_char >= '0' && next_char <= '9') ||
                                next_char == '+' || next_char == '&' ||
                                next_char == '%' || next_char == '\\' ||
                                next_char == '*' || next_char == '#' ||
                                next_char == '\r' || next_char == '\n') {
                                is_at_command = true;
                            }
                        }

                        if (is_at_command) {
                            MB_LOG_WARNING("Hayes filter: AT command BLOCKED in ONLINE mode: %.30s", ctx->line_buffer);
                        }
                    }
                }

                if (!is_at_command) {
                    /* Not an AT command - output the entire line */
                    for (size_t j = 0; j < ctx->line_len; j++) {
                        if (out_pos < output_size) {
                            output[out_pos++] = ctx->line_buffer[j];
                        }
                    }
                }
                /* Clear line buffer for next line */
                ctx->line_len = 0;
                memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
            } else {
                /* Not end of line - check for buffer overflow */
                if (ctx->line_len >= sizeof(ctx->line_buffer) - 1) {
                    /* Buffer full - flush it as it's probably not an AT command */
                    for (size_t j = 0; j < ctx->line_len; j++) {
                        if (out_pos < output_size) {
                            output[out_pos++] = ctx->line_buffer[j];
                        }
                    }
                    ctx->line_len = 0;
                    /* Add current character */
                    ctx->line_buffer[0] = c;
                    ctx->line_len = 1;
                }
            }

            ctx->last_char_time = current_time;
            continue;
        }

        /* COMMAND MODE - Original state machine for command processing */
        switch (ctx->state) {
            case HAYES_STATE_NORMAL:
                /* Command mode - accumulate line for AT command detection */
                /* Add to line buffer */
                size_t space_left = sizeof(ctx->line_buffer) - ctx->line_len - 1;
                if (space_left > 0) {
                    ctx->line_buffer[ctx->line_len++] = c;
                    ctx->line_buffer[ctx->line_len] = '\0';

                    /* Check for line ending */
                    bool has_line_ending = (c == '\r' || c == '\n');

                    if (has_line_ending) {
                        /* Process complete line */
                        if (ctx->line_len >= 3) { /* At least "AT" + line ending */
                            /* Check for AT command */
                            if ((ctx->line_buffer[0] == 'A' || ctx->line_buffer[0] == 'a') &&
                                (ctx->line_buffer[1] == 'T' || ctx->line_buffer[1] == 't')) {
                                /* AT command detected - process it */
                                /* Copy to command buffer for state machine */
                                memcpy(ctx->command_buffer, ctx->line_buffer, ctx->line_len);
                                ctx->command_len = ctx->line_len - 1; /* Exclude line ending */
                                ctx->state = HAYES_STATE_COMMAND;
                                MB_LOG_DEBUG("Hayes filter: AT command detected in line: %.20s", ctx->line_buffer);

                                /* Check if it's a known command */
                                if (l3_is_hayes_command(ctx->command_buffer, ctx->command_len, ctx->dict)) {
                                    /* Known command - filter it, wait for result */
                                    ctx->state = HAYES_STATE_CR_WAIT;
                                    MB_LOG_DEBUG("Hayes filter: Known command, waiting for result");
                                } else {
                                    /* Unknown command - pass through */
                                    for (size_t j = 0; j < ctx->line_len; j++) {
                                        if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                                    }
                                    ctx->state = HAYES_STATE_NORMAL;
                                }
                                ctx->command_len = 0;
                            } else {
                                /* Not an AT command - pass through */
                                for (size_t j = 0; j < ctx->line_len; j++) {
                                    if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                                }
                            }
                        } else {
                            /* Line too short for AT command - pass through */
                            for (size_t j = 0; j < ctx->line_len; j++) {
                                if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                            }
                        }
                        /* Clear line buffer */
                        ctx->line_len = 0;
                        memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
                    }
                    /* Otherwise continue buffering until line ending */
                } else {
                    /* Buffer overflow - flush and reset */
                    for (size_t j = 0; j < ctx->line_len; j++) {
                        if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                    }
                    if (out_pos < output_size) output[out_pos++] = c;
                    ctx->line_len = 0;
                    memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
                }
                break;

            case HAYES_STATE_CR_WAIT:
                /* Waiting for CR after command */
                if (c == '\r') {
                    ctx->state = HAYES_STATE_LF_WAIT;
                } else if (c == '\n') {
                    ctx->state = HAYES_STATE_RESULT;
                    ctx->result_len = 0;
                } else {
                    /* Unexpected - back to normal */
                    output[out_pos++] = c;
                    ctx->state = HAYES_STATE_NORMAL;
                }
                break;

            case HAYES_STATE_LF_WAIT:
                /* Waiting for LF after CR */
                if (c == '\n') {
                    ctx->state = HAYES_STATE_RESULT;
                    ctx->result_len = 0;
                } else {
                    /* Unexpected - back to normal */
                    output[out_pos++] = c;
                    ctx->state = HAYES_STATE_NORMAL;
                }
                break;

            case HAYES_STATE_RESULT:
                /* Accumulate result code */
                if (c == '\r' || c == '\n') {
                    /* Check if it's a known result */
                    if (l3_is_hayes_result(ctx->result_buffer, ctx->result_len, ctx->dict)) {
                        /* Check if this result switches to online mode */
                        for (size_t j = 0; j < ctx->dict->num_results; j++) {
                            if (memcmp(ctx->result_buffer, ctx->dict->results[j].code,
                                      strlen(ctx->dict->results[j].code)) == 0) {
                                if (ctx->dict->results[j].ends_command_mode) {
                                    ctx->in_online_mode = true;
                                    MB_LOG_INFO("Hayes filter: CONNECT detected, switching to ONLINE mode");
                                }
                                break;
                            }
                        }
                    } else {
                        /* Unknown result - pass through */
                        for (size_t j = 0; j < ctx->result_len; j++) {
                            output[out_pos++] = ctx->result_buffer[j];
                        }
                        output[out_pos++] = c;
                    }
                    ctx->state = HAYES_STATE_NORMAL;
                    ctx->result_len = 0;
                } else if (ctx->result_len < sizeof(ctx->result_buffer) - 1) {
                    ctx->result_buffer[ctx->result_len++] = c;
                }
                /* Don't output result characters */
                break;

            default:
                /* Unexpected state - reset */
                ctx->state = HAYES_STATE_NORMAL;
                output[out_pos++] = c;
                break;
        }

        ctx->last_char_time = current_time;
    }

    /* Note: No need to flush in ONLINE mode since we output immediately */
    /* The line buffer is only used for AT command detection, not for output buffering */

    *output_len = out_pos;
    return L3_SUCCESS;
}

int l3_filter_telnet_controls(telnet_filter_state_t *state, const unsigned char *input, size_t input_len,
                             unsigned char *output, size_t output_size, size_t *output_len)
{
    if (state == NULL || input == NULL || output == NULL || output_len == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    size_t out_pos = 0;
    telnet_filter_state_t current_state = *state;
    *output_len = 0;

    for (size_t i = 0; i < input_len && out_pos < output_size; i++) {
        unsigned char c = input[i];

        switch (current_state) {
            case TELNET_FILTER_STATE_DATA:
                if (c == 0xFF) { /* IAC (Interpret As Command) */
                    current_state = TELNET_FILTER_STATE_IAC;
                    MB_LOG_DEBUG("TELNET filter: Detected IAC character");
                } else {
                    /* Normal data - pass through */
                    output[out_pos++] = c;
                }
                break;

            case TELNET_FILTER_STATE_IAC:
                if (c == 0xFF) {
                    /* Escaped IAC - pass through literal 0xFF */
                    output[out_pos++] = c;
                    current_state = TELNET_FILTER_STATE_DATA;
                } else if (c == 0xFB) { /* WILL */
                    current_state = TELNET_FILTER_STATE_WILL;
                    MB_LOG_DEBUG("TELNET filter: WILL option");
                } else if (c == 0xFC) { /* WONT */
                    current_state = TELNET_FILTER_STATE_WONT;
                    MB_LOG_DEBUG("TELNET filter: WONT option");
                } else if (c == 0xFD) { /* DO */
                    current_state = TELNET_FILTER_STATE_DO;
                    MB_LOG_DEBUG("TELNET filter: DO option");
                } else if (c == 0xFE) { /* DONT */
                    current_state = TELNET_FILTER_STATE_DONT;
                    MB_LOG_DEBUG("TELNET filter: DONT option");
                } else if (c == 0xFA) { /* SB (Suboption Begin) */
                    current_state = TELNET_FILTER_STATE_SB;
                    MB_LOG_DEBUG("TELNET filter: Suboption begin");
                } else {
                    /* Other IAC commands - filter out */
                    current_state = TELNET_FILTER_STATE_DATA;
                }
                break;

            case TELNET_FILTER_STATE_WILL:
            case TELNET_FILTER_STATE_WONT:
            case TELNET_FILTER_STATE_DO:
            case TELNET_FILTER_STATE_DONT:
                /* Option negotiation - filter out the option byte */
                current_state = TELNET_FILTER_STATE_DATA;
                break;

            case TELNET_FILTER_STATE_SB:
                if (c == 0xFF) {
                    /* Potential IAC to end suboption */
                    current_state = TELNET_FILTER_STATE_SB_DATA;
                }
                /* Filter out suboption data */
                break;

            case TELNET_FILTER_STATE_SB_DATA:
                if (c == 0xF0) { /* SE (Suboption End) */
                    current_state = TELNET_FILTER_STATE_DATA;
                    MB_LOG_DEBUG("TELNET filter: Suboption end");
                } else {
                    /* Still in suboption */
                    current_state = TELNET_FILTER_STATE_SB;
                }
                break;
        }
    }

    *output_len = out_pos;
    *state = current_state;

    return L3_SUCCESS;
}

/* ========== Scheduling and Fairness ========== */

/* Enhanced scheduling and fairness functions */

/**
 * Initialize enhanced scheduling system with quantum enforcement
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_init_enhanced_scheduling(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        MB_LOG_ERROR("Invalid Level 3 context for scheduling initialization");
        return L3_ERROR_INVALID_PARAM;
    }

    MB_LOG_INFO("Initializing enhanced scheduling system with quantum enforcement");

    /* Initialize scheduling configuration */
    l3_ctx->sched_config.base_quantum_ms = 50;        /* 50ms base quantum */
    l3_ctx->sched_config.min_quantum_ms = 10;         /* 10ms minimum */
    l3_ctx->sched_config.max_quantum_ms = 200;        /* 200ms maximum */
    l3_ctx->sched_config.weight_balance_ratio = 0.6f; /* Favor serial slightly */
    l3_ctx->sched_config.starvation_threshold_ms = 500; /* 500ms starvation threshold */
    l3_ctx->sched_config.adaptive_quantum_enabled = true;
    l3_ctx->sched_config.fair_queue_enabled = true;

    /* Initialize latency tracking */
    l3_ctx->latency_stats.serial_to_telnet_avg_ms = 0.0;
    l3_ctx->latency_stats.telnet_to_serial_avg_ms = 0.0;
    l3_ctx->latency_stats.max_serial_to_telnet_ms = 0.0;
    l3_ctx->latency_stats.max_telnet_to_serial_ms = 0.0;
    l3_ctx->latency_stats.total_samples = 0;
    l3_ctx->latency_stats.last_measurement_time = 0;

    /* Initialize scheduling state */
    long long init_time = l3_get_timestamp_ms();
    l3_ctx->sched_state.current_direction = L3_PIPELINE_SERIAL_TO_TELNET;
    l3_ctx->sched_state.last_direction_switch_time = init_time;
    l3_ctx->sched_state.consecutive_slices = 0;
    l3_ctx->sched_state.serial_starvation_time = init_time;  /* Initialize to current time */
    l3_ctx->sched_state.telnet_starvation_time = init_time;  /* Initialize to current time */

    /* Initialize quantum state */
    l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.base_quantum_ms;
    l3_ctx->quantum_state.start_time = 0;
    l3_ctx->quantum_state.bytes_processed = 0;
    l3_ctx->quantum_state.max_bytes_per_quantum = 1024; /* Conservative start */

    /* Initialize fair queue weights */
    l3_ctx->fair_queue.serial_weight = 5;
    l3_ctx->fair_queue.telnet_weight = 5;
    l3_ctx->fair_queue.serial_deficit = 0;
    l3_ctx->fair_queue.telnet_deficit = 0;

    MB_LOG_INFO("Enhanced scheduling initialized - base quantum: %dms, balance ratio: %.2f",
                l3_ctx->sched_config.base_quantum_ms, l3_ctx->sched_config.weight_balance_ratio);

    return L3_SUCCESS;
}

l3_pipeline_direction_t l3_schedule_next_pipeline(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_PIPELINE_SERIAL_TO_TELNET;
    }

    pthread_mutex_lock(&l3_ctx->scheduling_mutex);

    /* Simple round-robin scheduling with fairness considerations */
    l3_pipeline_direction_t next_pipeline;

    /* Check if current pipeline has work to do */
    l3_pipeline_t *current = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                             &l3_ctx->pipeline_serial_to_telnet :
                             &l3_ctx->pipeline_telnet_to_serial;

    size_t current_available = l3_double_buffer_available(&current->buffers);

    /* If current pipeline has data and hasn't exceeded its timeslice, continue with it */
    if (current_available > 0 && current->bytes_in_timeslice < L3_MAX_BURST_SIZE) {
        next_pipeline = l3_ctx->active_pipeline;
    } else {
        /* Switch to the other pipeline */
        next_pipeline = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                        L3_PIPELINE_TELNET_TO_SERIAL :
                        L3_PIPELINE_SERIAL_TO_TELNET;

        /* Reset timeslice counter for new pipeline */
        l3_pipeline_t *next = (next_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                             &l3_ctx->pipeline_serial_to_telnet :
                             &l3_ctx->pipeline_telnet_to_serial;
        next->bytes_in_timeslice = 0;
        next->last_timeslice_start = time(NULL);

        l3_ctx->round_robin_counter++;
        MB_LOG_DEBUG("Fair scheduling: switched to pipeline %s (round #%d)",
                    l3_get_pipeline_name(next_pipeline), l3_ctx->round_robin_counter);
    }

    pthread_mutex_unlock(&l3_ctx->scheduling_mutex);
    return next_pipeline;
}

/**
 * Process pipeline with quantum enforcement and latency tracking
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction to process
 * @return SUCCESS on success, error code on failure
 */
static int l3_process_pipeline_with_quantum(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    long long current_time = l3_get_timestamp_ms();

    // Initialize quantum start time if needed
    if (l3_ctx->quantum_state.start_time == 0) {
        l3_ctx->quantum_state.start_time = current_time;
        l3_ctx->sched_state.last_direction_switch_time = current_time;
    }

    // Check if quantum has expired
    long long quantum_elapsed = current_time - l3_ctx->quantum_state.start_time;

    /* DEBUG: Log quantum timer status */
    static int quantum_debug_counter = 0;
    if (++quantum_debug_counter % 100 == 0) {
        /* printf("[DEBUG-QUANTUM-TIMER] elapsed=%lldms, quantum=%dms, will_expire=%d, start_time=%lld\n",
               quantum_elapsed, l3_ctx->quantum_state.current_quantum_ms,
               quantum_elapsed >= l3_ctx->quantum_state.current_quantum_ms,
               l3_ctx->quantum_state.start_time);
        fflush(stdout); */
    }

    /* DEBUG: Log when approaching quantum expiry */
    if (quantum_elapsed >= 45 && quantum_elapsed < l3_ctx->quantum_state.current_quantum_ms) {
        static int near_expiry_counter = 0;
        if (++near_expiry_counter % 50 == 0) {
            /* Commented out to reduce log noise
            printf("[DEBUG-NEAR-EXPIRY] elapsed=%lldms, quantum=%dms, current_time=%lld\n",
                   quantum_elapsed, l3_ctx->quantum_state.current_quantum_ms, current_time);
            fflush(stdout);
            */
        }
    }

    bool quantum_just_expired = false;
    if (quantum_elapsed >= l3_ctx->quantum_state.current_quantum_ms) {
        // Quantum expired - switch direction
        // CRITICAL FIX: Use current_direction, not the direction parameter!
        l3_pipeline_direction_t new_direction = (l3_ctx->sched_state.current_direction == L3_PIPELINE_SERIAL_TO_TELNET) ?
                                                L3_PIPELINE_TELNET_TO_SERIAL :
                                                L3_PIPELINE_SERIAL_TO_TELNET;

        /* DEBUG: Log the switch before changing values */
        /* Commented out to reduce log noise
        printf("[DEBUG-QUANTUM-SWITCH] Direction switching from %s to %s (elapsed=%lldms)\n",
               l3_get_direction_name(l3_ctx->sched_state.current_direction),
               l3_get_direction_name(new_direction),
               quantum_elapsed);
        fflush(stdout);
        */

        l3_ctx->sched_state.current_direction = new_direction;
        l3_ctx->active_pipeline = new_direction;  // Update the active pipeline
        l3_ctx->quantum_state.start_time = current_time;
        l3_ctx->quantum_state.bytes_processed = 0;
        l3_ctx->sched_state.consecutive_slices = 0;
        l3_ctx->last_pipeline_switch = time(NULL);  // Update switch time

        MB_LOG_DEBUG("Quantum expired: %dms, switching direction to %s",
                    (int)quantum_elapsed, l3_get_direction_name(new_direction));
        quantum_just_expired = true;
    }

    // Enforce latency boundaries - this is the core latency bound guarantee
    l3_enforce_latency_boundaries(l3_ctx);

    // Check if latency enforcement caused a direction switch
    // IMPORTANT: Don't return immediately if quantum just expired - process the new direction
    if (l3_ctx->sched_state.current_direction != direction && !quantum_just_expired) {
        MB_LOG_DEBUG("Latency enforcement caused direction switch: %s -> %s",
                    l3_get_direction_name(direction), l3_get_direction_name(l3_ctx->sched_state.current_direction));
        return L3_QUANTUM_EXPIRED;  // Signal that quantum processing should restart
    }

    // Check for starvation conditions
    if (l3_is_direction_starving(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET)) {
        l3_ctx->sched_state.current_direction = L3_PIPELINE_SERIAL_TO_TELNET;
        l3_ctx->active_pipeline = L3_PIPELINE_SERIAL_TO_TELNET;  // Update active pipeline
        l3_ctx->sched_state.serial_starvation_time = current_time;
        MB_LOG_DEBUG("Correcting serial starvation - forcing direction switch");
    } else if (l3_is_direction_starving(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL)) {
        l3_ctx->sched_state.current_direction = L3_PIPELINE_TELNET_TO_SERIAL;
        l3_ctx->active_pipeline = L3_PIPELINE_TELNET_TO_SERIAL;  // Update active pipeline
        l3_ctx->sched_state.telnet_starvation_time = current_time;
        MB_LOG_DEBUG("Correcting telnet starvation - forcing direction switch");
    }

    // Process data in the current direction (use the actual current direction, not the parameter)
    int ret = L3_SUCCESS;
    long long processing_start = l3_get_timestamp_ms();

    /* NOTE: Don't read current_direction yet - quantum switch may change it */
    l3_pipeline_direction_t current_direction;

    /* All direction changes (quantum, latency, starvation) are done above */
    /* NOW read the final direction to process */
    current_direction = l3_ctx->sched_state.current_direction;

    /* DEBUG: Log which direction is being processed */
    static int debug_counter = 0;
    if (++debug_counter % 10 == 0) {  /* Log every 10th iteration to reduce spam */
        /* Commented out to reduce log noise
        printf("[DEBUG-QUANTUM] Processing direction: %s (serial=%d, telnet=%d)\n",
               current_direction == L3_PIPELINE_SERIAL_TO_TELNET ? "SERIAL→TELNET" : "TELNET→SERIAL",
               L3_PIPELINE_SERIAL_TO_TELNET, L3_PIPELINE_TELNET_TO_SERIAL);
        fflush(stdout);
        */
    }

    if (current_direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        // Process serial to telnet pipeline
        ret = l3_process_serial_to_telnet_chunk(l3_ctx);
    } else {
        // Process telnet to serial pipeline
        ret = l3_process_telnet_to_serial_chunk(l3_ctx);
    }

    // Update latency statistics
    long long processing_time = l3_get_timestamp_ms() - processing_start;

    /* Update last service time AFTER processing to prevent false starvation detection */
    long long post_process_time = l3_get_timestamp_ms();
    if (current_direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        l3_ctx->sched_state.serial_starvation_time = post_process_time;
    } else {
        l3_ctx->sched_state.telnet_starvation_time = post_process_time;
    }
    l3_update_latency_stats(l3_ctx, current_direction, processing_time);

    // Update quantum state
    l3_ctx->quantum_state.bytes_processed++;
    l3_ctx->sched_state.consecutive_slices++;

    // Adaptive quantum adjustment
    if (l3_ctx->sched_config.adaptive_quantum_enabled) {
        l3_calculate_optimal_quantum(l3_ctx);
    }

    // Enforce latency boundaries (지연 상한 보장)
    l3_enforce_latency_boundaries(l3_ctx);

    // Check if we should force a direction switch due to latency
    if (l3_should_force_direction_switch(l3_ctx, current_direction)) {
        l3_pipeline_direction_t forced_direction = (current_direction == L3_PIPELINE_SERIAL_TO_TELNET) ?
                                                  L3_PIPELINE_TELNET_TO_SERIAL :
                                                  L3_PIPELINE_SERIAL_TO_TELNET;

        l3_ctx->sched_state.current_direction = forced_direction;
        l3_ctx->active_pipeline = forced_direction;
        /* NOTE: Do NOT reset start_time on forced switch - let quantum complete */
        /* l3_ctx->quantum_state.start_time = current_time; -- REMOVED */
        l3_ctx->quantum_state.bytes_processed = 0;
        l3_ctx->last_pipeline_switch = time(NULL);

        MB_LOG_DEBUG("Forced direction switch due to latency conditions: %s -> %s",
                    l3_get_direction_name(current_direction),
                    l3_get_direction_name(forced_direction));
    }

    return ret;
}

/**
 * Update latency statistics for performance monitoring
 */
static void l3_update_latency_stats(l3_context_t *l3_ctx, l3_pipeline_direction_t direction, long long latency_ms)
{
    if (!l3_ctx) {
        return;
    }

    // Update moving average based on direction
    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        if (l3_ctx->latency_stats.serial_to_telnet_avg_ms == 0.0) {
            l3_ctx->latency_stats.serial_to_telnet_avg_ms = (double)latency_ms;
        } else {
            // Exponential moving average with alpha = 0.1
            l3_ctx->latency_stats.serial_to_telnet_avg_ms =
                0.9 * l3_ctx->latency_stats.serial_to_telnet_avg_ms + 0.1 * (double)latency_ms;
        }

        // Update maximum
        if (latency_ms > l3_ctx->latency_stats.max_serial_to_telnet_ms) {
            l3_ctx->latency_stats.max_serial_to_telnet_ms = latency_ms;
        }
    } else {
        if (l3_ctx->latency_stats.telnet_to_serial_avg_ms == 0.0) {
            l3_ctx->latency_stats.telnet_to_serial_avg_ms = (double)latency_ms;
        } else {
            l3_ctx->latency_stats.telnet_to_serial_avg_ms =
                0.9 * l3_ctx->latency_stats.telnet_to_serial_avg_ms + 0.1 * (double)latency_ms;
        }

        // Update maximum
        if (latency_ms > l3_ctx->latency_stats.max_telnet_to_serial_ms) {
            l3_ctx->latency_stats.max_telnet_to_serial_ms = latency_ms;
        }
    }

    l3_ctx->latency_stats.total_samples++;
    l3_ctx->latency_stats.last_measurement_time = l3_get_timestamp_ms();

    // Log warning if latency exceeds threshold
    const long long LATENCY_WARNING_THRESHOLD = 100; // 100ms
    if (latency_ms > LATENCY_WARNING_THRESHOLD) {
        MB_LOG_WARNING("High latency detected: %s direction, %lldms",
                      l3_get_direction_name(direction), latency_ms);
    }
}

/**
 * Check if a direction is experiencing starvation
 * @param l3_ctx Level 3 context
 * @param direction Direction to check
 * @return true if direction is starving, false otherwise
 */
static bool l3_is_direction_starving(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return false;
    }

    long long current_time = l3_get_timestamp_ms();
    long long time_since_last_service = 0;

    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        time_since_last_service = current_time - l3_ctx->sched_state.serial_starvation_time;
    } else {
        time_since_last_service = current_time - l3_ctx->sched_state.telnet_starvation_time;
    }

    return (time_since_last_service > l3_ctx->sched_config.starvation_threshold_ms);
}

/**
 * Calculate optimal quantum based on current system conditions
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_calculate_optimal_quantum(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Calculate quantum based on latency and throughput */
    double avg_latency = (l3_ctx->latency_stats.serial_to_telnet_avg_ms +
                         l3_ctx->latency_stats.telnet_to_serial_avg_ms) / 2.0;

    /* Adjust quantum inversely proportional to latency */
    int target_quantum = l3_ctx->sched_config.base_quantum_ms;

    if (avg_latency > 50.0) {  /* High latency - reduce quantum */
        target_quantum = (int)(l3_ctx->sched_config.base_quantum_ms * 0.8);
    } else if (avg_latency < 10.0) {  /* Low latency - can increase quantum */
        target_quantum = (int)(l3_ctx->sched_config.base_quantum_ms * 1.2);
    }

    /* Clamp to min/max bounds */
    if (target_quantum < l3_ctx->sched_config.min_quantum_ms) {
        target_quantum = l3_ctx->sched_config.min_quantum_ms;
    } else if (target_quantum > l3_ctx->sched_config.max_quantum_ms) {
        target_quantum = l3_ctx->sched_config.max_quantum_ms;
    }

    /* Update if significantly different from current */
    if (abs(target_quantum - l3_ctx->quantum_state.current_quantum_ms) > 5) {
        MB_LOG_DEBUG("Adaptive quantum: %dms -> %dms (avg latency: %.2fms)",
                    l3_ctx->quantum_state.current_quantum_ms, target_quantum, avg_latency);
        l3_ctx->quantum_state.current_quantum_ms = target_quantum;
    }

    return L3_SUCCESS;
}

/**
 * Update fair queue weights based on recent performance
 */
static int l3_update_fair_queue_weights(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    // Calculate performance ratio
    double serial_latency = l3_ctx->latency_stats.serial_to_telnet_avg_ms;
    double telnet_latency = l3_ctx->latency_stats.telnet_to_serial_avg_ms;

    if (serial_latency > 0.0 && telnet_latency > 0.0) {
        // Adjust weights to balance latency
        double latency_ratio = serial_latency / telnet_latency;

        if (latency_ratio > 1.2) {  // Serial latency higher - give it more weight
            l3_ctx->fair_queue.serial_weight = 6;
            l3_ctx->fair_queue.telnet_weight = 4;
        } else if (latency_ratio < 0.8) {  // Telnet latency higher - give it more weight
            l3_ctx->fair_queue.serial_weight = 4;
            l3_ctx->fair_queue.telnet_weight = 6;
        } else {  // Balanced
            l3_ctx->fair_queue.serial_weight = 5;
            l3_ctx->fair_queue.telnet_weight = 5;
        }

        MB_LOG_DEBUG("Fair queue weights updated - Serial: %d, Telnet: %d (latency ratio: %.2f)",
                    l3_ctx->fair_queue.serial_weight, l3_ctx->fair_queue.telnet_weight, latency_ratio);
    }

    return L3_SUCCESS;
}

/**
 * Get comprehensive scheduling statistics
 */
static int l3_get_scheduling_statistics(l3_context_t *l3_ctx, l3_scheduling_stats_t *stats)
{
    if (!l3_ctx || !stats) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(stats, 0, sizeof(l3_scheduling_stats_t));

    // Copy available statistics that match header definition
    // Note: Only copy fields that exist in the header structure

    // Copy latency statistics
    stats->avg_latency_ms[0] = l3_ctx->latency_stats.serial_to_telnet_avg_ms;
    stats->avg_latency_ms[1] = l3_ctx->latency_stats.telnet_to_serial_avg_ms;
    stats->max_latency_samples[0] = (int)l3_ctx->latency_stats.max_serial_to_telnet_ms;
    stats->max_latency_samples[1] = (int)l3_ctx->latency_stats.max_telnet_to_serial_ms;
    stats->latency_exceedances[0] = 0;  // Could be implemented if needed
    stats->latency_exceedances[1] = 0;

    // Copy byte statistics
    stats->bytes_processed[0] = l3_ctx->pipeline_serial_to_telnet.total_bytes_processed;
    stats->bytes_processed[1] = l3_ctx->pipeline_telnet_to_serial.total_bytes_processed;
    stats->quantum_count[0] = 0;  // Could be tracked if needed
    stats->quantum_count[1] = 0;
    stats->avg_quantum_size[0] = 0;  // Could be calculated if needed
    stats->avg_quantum_size[1] = 0;

    // Copy fairness statistics
    stats->consecutive_slices[0] = l3_ctx->sched_state.consecutive_slices;
    stats->consecutive_slices[1] = 0;  // Other direction
    stats->forced_slices[0] = 0;  // Could be tracked if needed
    stats->forced_slices[1] = 0;
    stats->starvations_detected[0] = 0;  // Could be tracked if needed
    stats->starvations_detected[1] = 0;

    // Calculate performance metrics
    stats->fairness_ratio = 1.0;  // Could be calculated based on actual metrics
    stats->system_utilization = l3_get_system_utilization(l3_ctx) / 100.0;
    stats->total_scheduling_cycles = l3_ctx->round_robin_counter;
    stats->last_update_time = time(NULL);

    return L3_SUCCESS;
}

/* ========== Latency Bound Guarantee Functions ========== */

/**
 * Enforce latency boundaries for both pipeline directions
 * This is the core function for latency bound guarantee implementation
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_enforce_latency_boundaries(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* current_time removed - no longer needed after removing start_time resets */
    int latency_violations = 0;

    /* Check Serial→Telnet direction latency */
    if (l3_detect_latency_violation(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET)) {
        latency_violations++;
        MB_LOG_WARNING("Serial→Telnet latency bound violation detected");

        /* Force direction switch to Serial→Telnet if it's been waiting too long */
        if (l3_should_force_direction_switch(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET)) {
            l3_ctx->sched_state.current_direction = L3_PIPELINE_SERIAL_TO_TELNET;
            l3_ctx->active_pipeline = L3_PIPELINE_SERIAL_TO_TELNET;
            /* NOTE: Do NOT reset start_time - let quantum complete naturally */
            /* l3_ctx->quantum_state.start_time = current_time; -- REMOVED */
            l3_ctx->quantum_state.bytes_processed = 0;

            MB_LOG_INFO("Forced switch to Serial→Telnet due to latency violation");
        }
    }

    /* Check Telnet→Serial direction latency */
    if (l3_detect_latency_violation(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL)) {
        latency_violations++;
        MB_LOG_WARNING("Telnet→Serial latency bound violation detected");

        /* Force direction switch to Telnet→Serial if it's been waiting too long */
        if (l3_should_force_direction_switch(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL)) {
            l3_ctx->sched_state.current_direction = L3_PIPELINE_TELNET_TO_SERIAL;
            l3_ctx->active_pipeline = L3_PIPELINE_TELNET_TO_SERIAL;
            /* NOTE: Do NOT reset start_time - let quantum complete naturally */
            /* l3_ctx->quantum_state.start_time = current_time; -- REMOVED */
            l3_ctx->quantum_state.bytes_processed = 0;

            MB_LOG_INFO("Forced switch to Telnet→Serial due to latency violation");
        }
    }

    /* If latency violations detected, update priorities and quantum */
    if (latency_violations > 0) {
        l3_update_direction_priorities(l3_ctx);
        l3_calculate_adaptive_quantum_with_latency(l3_ctx);
    }

    return L3_SUCCESS;
}

/**
 * Detect latency violation for a specific pipeline direction
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction to check
 * @return true if latency violation detected, false otherwise
 */
static int l3_detect_latency_violation(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return false;
    }

    long long wait_time = l3_get_direction_wait_time(l3_ctx, direction);
    int latency_bound_ms = l3_ctx->sched_config.latency_bound_ms;

    /* For 1200 bps environment, use more relaxed bounds */
    if (l3_ctx->system_config.serial_baudrate <= 2400) {
        latency_bound_ms *= 2;  /* Double the latency bound for low speed */
    }

    /* Check if wait time exceeds latency bound */
    if (wait_time > latency_bound_ms) {
        MB_LOG_DEBUG("Latency violation detected for %s: wait_time=%lldms, bound=%dms",
                    l3_get_direction_name(direction), wait_time, latency_bound_ms);
        return true;
    }

    return false;
}

/**
 * Calculate adaptive quantum with latency consideration
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_calculate_adaptive_quantum_with_latency(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* current_time removed - not needed after removing start_time reset */
    int base_quantum = l3_ctx->sched_config.base_quantum_ms;

    /* Get wait times for both directions */
    long long serial_wait = l3_get_direction_wait_time(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET);
    long long telnet_wait = l3_get_direction_wait_time(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL);

    /* Calculate wait time ratio for fairness adjustment */
    long long max_wait = (serial_wait > telnet_wait) ? serial_wait : telnet_wait;
    long long min_wait = (serial_wait < telnet_wait) ? serial_wait : telnet_wait;

    double wait_ratio = 1.0;
    if (min_wait > 0) {
        wait_ratio = (double)max_wait / min_wait;
    }

    /* Adaptive quantum calculation:
     * - If one direction is waiting much longer, reduce quantum for current direction
     * - If wait times are balanced, use normal quantum
     * - Extreme imbalance: use minimum quantum
     */
    int adaptive_quantum = base_quantum;

    if (wait_ratio > 3.0) {
        /* Extreme imbalance - use minimum quantum */
        adaptive_quantum = l3_ctx->sched_config.min_quantum_ms;
        MB_LOG_DEBUG("Extreme wait imbalance (ratio=%.2f) - using minimum quantum: %dms",
                    wait_ratio, adaptive_quantum);
    } else if (wait_ratio > 1.5) {
        /* Moderate imbalance - reduce quantum */
        adaptive_quantum = (int)(base_quantum * 0.7);
        MB_LOG_DEBUG("Moderate wait imbalance (ratio=%.2f) - reduced quantum: %dms",
                    wait_ratio, adaptive_quantum);
    } else {
        /* Balanced - use normal quantum */
        adaptive_quantum = base_quantum;
    }

    /* Apply latency-specific adjustments for low-speed environments */
    if (l3_ctx->system_config.serial_baudrate <= 2400) {
        /* For 1200/2400 bps, ensure minimum processing time */
        int min_low_speed_quantum = (l3_ctx->sched_config.latency_bound_ms / 4);
        if (adaptive_quantum < min_low_speed_quantum) {
            adaptive_quantum = min_low_speed_quantum;
        }
    }

    /* Clamp to valid range */
    if (adaptive_quantum < l3_ctx->sched_config.min_quantum_ms) {
        adaptive_quantum = l3_ctx->sched_config.min_quantum_ms;
    } else if (adaptive_quantum > l3_ctx->sched_config.max_quantum_ms) {
        adaptive_quantum = l3_ctx->sched_config.max_quantum_ms;
    }

    /* Update current quantum if significantly different */
    if (abs(adaptive_quantum - l3_ctx->quantum_state.current_quantum_ms) >
        (l3_ctx->sched_config.min_quantum_ms / 2)) {

        MB_LOG_INFO("Adaptive quantum updated: %dms -> %dms (wait_ratio=%.2f, serial_wait=%lldms, telnet_wait=%lldms)",
                    l3_ctx->quantum_state.current_quantum_ms, adaptive_quantum,
                    wait_ratio, serial_wait, telnet_wait);

        l3_ctx->quantum_state.current_quantum_ms = adaptive_quantum;
        /* NOTE: Do NOT reset start_time here - let current quantum complete naturally */
        /* l3_ctx->quantum_state.start_time = current_time; -- REMOVED */
        l3_ctx->quantum_state.bytes_processed = 0;
    }

    return L3_SUCCESS;
}

/**
 * Update direction priorities based on wait times and latency
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_update_direction_priorities(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    long long serial_wait = l3_get_direction_wait_time(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET);
    long long telnet_wait = l3_get_direction_wait_time(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL);

    /* Calculate priority weights based on wait times */
    /* Longer wait time = higher priority */
    double total_wait = (double)(serial_wait + telnet_wait);
    if (total_wait == 0.0) {
        /* No wait time difference - keep balanced priorities */
        l3_ctx->fair_queue.serial_weight = 5;
        l3_ctx->fair_queue.telnet_weight = 5;
        return L3_SUCCESS;
    }

    /* Calculate normalized priorities (0-10 scale) */
    int serial_priority = (int)((serial_wait / total_wait) * 10.0);
    int telnet_priority = (int)((telnet_wait / total_wait) * 10.0);

    /* Ensure minimum priority of 1 for both directions */
    if (serial_priority == 0) serial_priority = 1;
    if (telnet_priority == 0) telnet_priority = 1;

    /* Normalize to ensure sum equals 10 */
    int priority_sum = serial_priority + telnet_priority;
    if (priority_sum != 10) {
        serial_priority = (serial_priority * 10) / priority_sum;
        telnet_priority = 10 - serial_priority;
    }

    l3_ctx->fair_queue.serial_weight = serial_priority;
    l3_ctx->fair_queue.telnet_weight = telnet_priority;

    MB_LOG_DEBUG("Direction priorities updated - Serial: %d, Telnet: %d (wait_times: %lldms, %lldms)",
                serial_priority, telnet_priority, serial_wait, telnet_wait);

    return L3_SUCCESS;
}

/**
 * Get direction wait time (time since last service)
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction
 * @return Wait time in milliseconds
 */
static long long l3_get_direction_wait_time(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return 0;
    }

    long long current_time = l3_get_timestamp_ms();
    long long last_service_time = 0;

    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        last_service_time = l3_ctx->sched_state.serial_starvation_time;
    } else {
        last_service_time = l3_ctx->sched_state.telnet_starvation_time;
    }

    if (last_service_time == 0) {
        /* No service recorded yet - assume system start time */
        last_service_time = l3_ctx->system_start_time;
    }

    long long wait_time = current_time - last_service_time;
    return (wait_time > 0) ? wait_time : 0;
}

/**
 * Determine if direction switch should be forced due to latency
 * @param l3_ctx Level 3 context
 * @param direction Direction that might need forced service
 * @return true if forced switch recommended, false otherwise
 */
static bool l3_should_force_direction_switch(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return false;
    }

    /* Don't force switch if already processing this direction */
    if (l3_ctx->sched_state.current_direction == direction) {
        return false;
    }

    long long wait_time = l3_get_direction_wait_time(l3_ctx, direction);
    int latency_bound = l3_ctx->sched_config.latency_bound_ms;

    /* For low-speed environments, use more lenient thresholds */
    if (l3_ctx->system_config.serial_baudrate <= 2400) {
        latency_bound = latency_bound * 3 / 2;  /* 1.5x more lenient */
    }

    /* Force switch if wait time exceeds 1.5x latency bound */
    if (wait_time > (latency_bound * 3 / 2)) {
        MB_LOG_DEBUG("Force switch condition met for %s: wait_time=%lldms, threshold=%dms",
                    l3_get_direction_name(direction), wait_time, (latency_bound * 3 / 2));
        return true;
    }

    /* Also consider starvation conditions */
    if (l3_is_direction_starving(l3_ctx, direction)) {
        MB_LOG_DEBUG("Force switch due to starvation for %s", l3_get_direction_name(direction));
        return true;
    }

    return false;
}

/* ========== Enhanced Buffer Management - LEVEL3_WORK_TODO.txt Compliant ========== */

/**
 * Initialize enhanced double buffer with watermark defense
 * @param ebuf Enhanced double buffer to initialize
 * @param initial_size Initial buffer size
 * @param min_size Minimum buffer size
 * @param max_size Maximum buffer size
 * @return SUCCESS on success, error code on failure
 */
int l3_enhanced_double_buffer_init(l3_enhanced_double_buffer_t *ebuf,
                                   size_t initial_size, size_t min_size, size_t max_size)
{
    if (!ebuf || initial_size == 0 || min_size == 0 || max_size == 0) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (min_size > max_size || initial_size < min_size || initial_size > max_size) {
        MB_LOG_ERROR("Invalid buffer size parameters: initial=%zu, min=%zu, max=%zu",
                    initial_size, min_size, max_size);
        return L3_ERROR_INVALID_PARAM;
    }

    memset(ebuf, 0, sizeof(l3_enhanced_double_buffer_t));

    /* Allocate buffers dynamically */
    ebuf->main_data = malloc(initial_size);
    ebuf->sub_data = malloc(initial_size);

    if (!ebuf->main_data || !ebuf->sub_data) {
        MB_LOG_ERROR("Failed to allocate enhanced buffer memory");
        free(ebuf->main_data);
        free(ebuf->sub_data);
        return L3_ERROR_MEMORY;
    }

    /* Initialize buffer configuration */
    ebuf->buffer_size = initial_size;
    ebuf->config.min_buffer_size = min_size;
    ebuf->config.max_buffer_size = max_size;
    ebuf->config.current_buffer_size = initial_size;

    /* Set default watermark levels */
    ebuf->config.critical_watermark = (size_t)(max_size * 0.95);
    ebuf->config.high_watermark = (size_t)(max_size * 0.80);
    ebuf->config.low_watermark = (size_t)(max_size * 0.20);
    ebuf->config.empty_watermark = (size_t)(max_size * 0.05);

    /* Enable adaptive features */
    ebuf->config.adaptive_sizing_enabled = true;
    ebuf->config.backpressure_enabled = true;
    ebuf->config.flow_control_enabled = true;

    /* Set adaptive sizing parameters */
    ebuf->config.growth_threshold = 85;      /* Grow when >85% full */
    ebuf->config.shrink_threshold = 15;      /* Shrink when <15% full */
    ebuf->config.growth_step_size = 1024;    /* Grow in 1KB steps */
    ebuf->config.shrink_step_size = 512;     /* Shrink in 512B steps */

    /* Initialize metrics */
    ebuf->metrics.current_usage = 0;
    ebuf->metrics.peak_usage = 0;
    ebuf->metrics.min_free_space = initial_size;
    ebuf->metrics.current_level = L3_WATERMARK_EMPTY;
    ebuf->metrics.peak_level = L3_WATERMARK_EMPTY;
    ebuf->metrics.avg_fill_ratio = 0.0;

    /* Initialize watermark state */
    ebuf->current_watermark = L3_WATERMARK_EMPTY;
    ebuf->watermark_change_time = time(NULL);
    ebuf->backpressure_active = false;

    /* Initialize dynamic sizing state */
    ebuf->last_resize_time = time(NULL);
    ebuf->consecutive_overflows = 0;
    ebuf->consecutive_underflows = 0;

    /* Initialize buffer switching */
    ebuf->main_active = true;
    ebuf->main_len = 0;
    ebuf->main_pos = 0;
    ebuf->sub_len = 0;

    /* Initialize mutex */
    if (pthread_mutex_init(&ebuf->mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize enhanced buffer mutex");
        free(ebuf->main_data);
        free(ebuf->sub_data);
        return L3_ERROR_THREAD;
    }

    /* Initialize memory pool */
    int ret = l3_memory_pool_init(&ebuf->memory_pool,
                                  max_size * 2,  /* Pool size = 2x max buffer */
                                  512);         /* Block size = 512 bytes */
    if (ret != L3_SUCCESS) {
        MB_LOG_WARNING("Failed to initialize memory pool, continuing without it");
    }

    MB_LOG_INFO("Enhanced buffer initialized: size=%zu, min=%zu, max=%zu",
                initial_size, min_size, max_size);

    return L3_SUCCESS;
}

/**
 * Cleanup enhanced double buffer and free memory
 * @param ebuf Enhanced double buffer to cleanup
 */
void l3_enhanced_double_buffer_cleanup(l3_enhanced_double_buffer_t *ebuf)
{
    if (!ebuf) {
        return;
    }

    /* Cleanup memory pool */
    l3_memory_pool_cleanup(&ebuf->memory_pool);

    /* Free allocated buffers */
    free(ebuf->main_data);
    free(ebuf->sub_data);

    /* Cleanup mutex */
    pthread_mutex_destroy(&ebuf->mutex);

    memset(ebuf, 0, sizeof(l3_enhanced_double_buffer_t));

    MB_LOG_DEBUG("Enhanced buffer cleaned up");
}

/**
 * Get current watermark level for buffer
 * @param ebuf Enhanced double buffer context
 * @return Current watermark level
 */
l3_watermark_level_t l3_get_watermark_level(l3_enhanced_double_buffer_t *ebuf)
{
    if (!ebuf) {
        return L3_WATERMARK_EMPTY;
    }

    size_t total_usage = ebuf->main_len + ebuf->sub_len;
    double fill_ratio = (double)total_usage / (ebuf->buffer_size * 2);

    if (fill_ratio > 0.95) {
        return L3_WATERMARK_CRITICAL;
    } else if (fill_ratio > 0.80) {
        return L3_WATERMARK_HIGH;
    } else if (fill_ratio > 0.20) {
        return L3_WATERMARK_NORMAL;
    } else if (fill_ratio > 0.05) {
        return L3_WATERMARK_LOW;
    } else {
        return L3_WATERMARK_EMPTY;
    }
}

/**
 * Write data to enhanced buffer with watermark protection
 * Enhanced with BACKPRESSURE.txt compliant high/low watermark logic
 * @param ebuf Enhanced double buffer context
 * @param data Data to write
 * @param len Data length
 * @return Number of bytes written (may be 0 if full or backpressure)
 */
size_t l3_enhanced_double_buffer_write(l3_enhanced_double_buffer_t *ebuf,
                                       const unsigned char *data, size_t len)
{
    if (!ebuf || !data || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&ebuf->mutex);

    /* Check current watermark level */
    l3_watermark_level_t current_level = l3_get_watermark_level(ebuf);

    /* Enhanced backpressure check with hysteresis logic */
    if (ebuf->config.backpressure_enabled) {
        bool should_block = l3_should_apply_enhanced_backpressure(ebuf);

        if (should_block) {
            ebuf->metrics.overflow_events++;
            ebuf->consecutive_overflows++;
            ebuf->metrics.bytes_dropped += len;

            MB_LOG_DEBUG("Backpressure active - dropping %zu bytes (level: %s, overflow #%d)",
                        len, l3_watermark_level_to_string(current_level),
                        ebuf->consecutive_overflows);

            pthread_mutex_unlock(&ebuf->mutex);
            return 0;  /* Drop all data when backpressure active */
        }
    }

    /* Check available space in sub-buffer */
    size_t available_space = ebuf->buffer_size - ebuf->sub_len;
    size_t to_write = len < available_space ? len : available_space;

    if (to_write > 0) {
        memcpy(ebuf->sub_data + ebuf->sub_len, data, to_write);
        ebuf->sub_len += to_write;
        ebuf->last_activity = time(NULL);

        /* Reset consecutive overflows on successful write */
        ebuf->consecutive_overflows = 0;

        MB_LOG_DEBUG("Successfully wrote %zu bytes (level: %s)",
                    to_write, l3_watermark_level_to_string(current_level));
    } else {
        /* Buffer full - count as overflow even if not at critical level */
        ebuf->metrics.overflow_events++;
        ebuf->consecutive_overflows++;
        ebuf->metrics.bytes_dropped += len;

        MB_LOG_DEBUG("Buffer full - dropping %zu bytes (level: %s, overflow #%d)",
                    len, l3_watermark_level_to_string(current_level),
                    ebuf->consecutive_overflows);
    }

    /* Update watermark level and metrics */
    l3_watermark_level_t new_level = l3_get_watermark_level(ebuf);
    if (new_level != current_level) {
        ebuf->current_watermark = new_level;
        ebuf->watermark_change_time = time(NULL);
        MB_LOG_DEBUG("Watermark level changed: %s -> %s",
                    l3_watermark_level_to_string(current_level),
                    l3_watermark_level_to_string(new_level));
    }

    l3_update_buffer_metrics(ebuf, to_write, 0);

    pthread_mutex_unlock(&ebuf->mutex);
    return to_write;
}

/**
 * Read data from enhanced buffer
 * @param ebuf Enhanced double buffer context
 * @param data Output buffer
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
size_t l3_enhanced_double_buffer_read(l3_enhanced_double_buffer_t *ebuf,
                                      unsigned char *data, size_t len)
{
    if (!ebuf || !data || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&ebuf->mutex);

    /* Read from main buffer */
    size_t available = ebuf->main_len - ebuf->main_pos;
    size_t to_read = len < available ? len : available;

    if (to_read > 0) {
        memcpy(data, ebuf->main_data + ebuf->main_pos, to_read);
        ebuf->main_pos += to_read;
        ebuf->bytes_processed += to_read;
        ebuf->last_activity = time(NULL);

        /* Reset consecutive underflows on successful read */
        ebuf->consecutive_underflows = 0;
    } else {
        /* No data available - count as underflow */
        ebuf->metrics.underflow_events++;
        ebuf->consecutive_underflows++;

        MB_LOG_DEBUG("Buffer empty - underflow #%d", ebuf->consecutive_underflows);
    }

    /* Update metrics */
    l3_update_buffer_metrics(ebuf, 0, to_read);

    pthread_mutex_unlock(&ebuf->mutex);
    return to_read;
}

/**
 * Check and apply backpressure based on watermark level
 * Enhanced with hysteresis per BACKPRESSURE.txt principles
 * @param ebuf Enhanced double buffer context
 * @return true if backpressure should be applied
 */
bool l3_should_apply_enhanced_backpressure(l3_enhanced_double_buffer_t *ebuf)
{
    if (!ebuf || !ebuf->config.backpressure_enabled) {
        return false;
    }

    l3_watermark_level_t level = l3_get_watermark_level(ebuf);
    bool should_apply = false;

    /* BACKPRESSURE.txt compliant hysteresis logic:
     * - Apply backpressure at HIGH watermark (80%) to prevent overflow
     * - Release backpressure at LOW watermark (20%) to ensure recovery
     * This creates the necessary hysteresis to prevent oscillation
     */
    if (ebuf->backpressure_active) {
        /* Currently in backpressure - release only when below LOW watermark */
        should_apply = (level != L3_WATERMARK_LOW && level != L3_WATERMARK_EMPTY);

        if (!should_apply) {
            MB_LOG_DEBUG("Backpressure released: level=%s (below LOW watermark)",
                        l3_watermark_level_to_string(level));
        }
    } else {
        /* Currently not in backpressure - apply at HIGH or above */
        should_apply = (level == L3_WATERMARK_HIGH || level == L3_WATERMARK_CRITICAL);

        if (should_apply) {
            MB_LOG_DEBUG("Backpressure applied: level=%s (at HIGH watermark)",
                        l3_watermark_level_to_string(level));
        }
    }

    /* Update backpressure state if changed */
    if (should_apply != ebuf->backpressure_active) {
        ebuf->backpressure_active = should_apply;
        ebuf->watermark_change_time = time(NULL);

        MB_LOG_INFO("Backpressure state changed: %s -> %s (level: %s)",
                    should_apply ? "RELEASED" : "APPLIED",
                    should_apply ? "APPLIED" : "RELEASED",
                    l3_watermark_level_to_string(level));
    }

    return should_apply;
}

/**
 * Get watermark level name as string
 * @param level Watermark level
 * @return String representation of level
 */
const char *l3_watermark_level_to_string(l3_watermark_level_t level)
{
    switch (level) {
        case L3_WATERMARK_CRITICAL: return "CRITICAL";
        case L3_WATERMARK_HIGH:     return "HIGH";
        case L3_WATERMARK_NORMAL:   return "NORMAL";
        case L3_WATERMARK_LOW:      return "LOW";
        case L3_WATERMARK_EMPTY:    return "EMPTY";
        default:                     return "UNKNOWN";
    }
}

/**
 * Resize enhanced buffer dynamically - BACKPRESSURE.txt compliant
 * @param ebuf Enhanced double buffer context
 * @param new_size New buffer size
 * @return SUCCESS on success, error code on failure
 */
int l3_resize_buffer(l3_enhanced_double_buffer_t *ebuf, size_t new_size)
{
    if (!ebuf || new_size == 0) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Validate new size against configured bounds */
    if (new_size < ebuf->config.min_buffer_size || new_size > ebuf->config.max_buffer_size) {
        MB_LOG_ERROR("Invalid resize target: %zu bytes (min: %zu, max: %zu)",
                    new_size, ebuf->config.min_buffer_size, ebuf->config.max_buffer_size);
        return L3_ERROR_INVALID_PARAM;
    }

    /* No resize needed */
    if (new_size == ebuf->buffer_size) {
        MB_LOG_DEBUG("Resize not needed: already %zu bytes", new_size);
        return L3_SUCCESS;
    }

    MB_LOG_INFO("Resizing enhanced buffer: %zu -> %zu bytes", ebuf->buffer_size, new_size);

    /* Allocate new buffers */
    unsigned char *new_main_data = malloc(new_size);
    unsigned char *new_sub_data = malloc(new_size);

    if (!new_main_data || !new_sub_data) {
        MB_LOG_ERROR("Failed to allocate memory for buffer resize");
        free(new_main_data);
        free(new_sub_data);
        return L3_ERROR_MEMORY;
    }

    /* Copy existing data to new buffers */
    size_t main_copy_len = ebuf->main_len < new_size ? ebuf->main_len : new_size;
    size_t sub_copy_len = ebuf->sub_len < new_size ? ebuf->sub_len : new_size;

    if (main_copy_len > 0) {
        memcpy(new_main_data, ebuf->main_data, main_copy_len);
    }
    if (sub_copy_len > 0) {
        memcpy(new_sub_data, ebuf->sub_data, sub_copy_len);
    }

    /* Handle data truncation if shrinking */
    if (new_size < ebuf->buffer_size) {
        if (ebuf->main_len > new_size) {
            ebuf->main_len = new_size;
            MB_LOG_WARNING("Main buffer truncated: %zu -> %zu bytes", ebuf->main_len, new_size);
        }
        if (ebuf->sub_len > new_size) {
            ebuf->sub_len = new_size;
            MB_LOG_WARNING("Sub buffer truncated: %zu -> %zu bytes", ebuf->sub_len, new_size);
        }
        /* Adjust read position if beyond new buffer size */
        if (ebuf->main_pos >= new_size) {
            ebuf->main_pos = 0;  /* Reset to beginning if truncated past current position */
        }
    }

    /* Free old buffers and update pointers */
    free(ebuf->main_data);
    free(ebuf->sub_data);

    ebuf->main_data = new_main_data;
    ebuf->sub_data = new_sub_data;
    ebuf->buffer_size = new_size;
    ebuf->config.current_buffer_size = new_size;

    /* Update watermark thresholds based on new size */
    ebuf->config.critical_watermark = (size_t)(new_size * 0.95);
    ebuf->config.high_watermark = (size_t)(new_size * 0.80);
    ebuf->config.low_watermark = (size_t)(new_size * 0.20);
    ebuf->config.empty_watermark = (size_t)(new_size * 0.05);

    MB_LOG_INFO("Buffer resize completed: %zu bytes, watermarks updated (critical: %zu, high: %zu, low: %zu)",
                new_size, ebuf->config.critical_watermark, ebuf->config.high_watermark, ebuf->config.low_watermark);

    return L3_SUCCESS;
}

/**
 * Update buffer metrics after operation with dynamic resizing
 * @param ebuf Enhanced double buffer context
 * @param bytes_written Number of bytes written (0 if read operation)
 * @param bytes_read Number of bytes read (0 if write operation)
 * @return SUCCESS on success, error code on failure
 */
int l3_update_buffer_metrics(l3_enhanced_double_buffer_t *ebuf,
                             size_t bytes_written, size_t bytes_read)
{
    if (!ebuf) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Suppress unused parameter warnings */
    (void)bytes_written;
    (void)bytes_read;

    /* Update current usage */
    ebuf->metrics.current_usage = ebuf->main_len + ebuf->sub_len;

    /* Update peak usage */
    if (ebuf->metrics.current_usage > ebuf->metrics.peak_usage) {
        ebuf->metrics.peak_usage = ebuf->metrics.current_usage;
    }

    /* Update minimum free space */
    size_t total_capacity = ebuf->buffer_size * 2;
    size_t current_free = total_capacity - ebuf->metrics.current_usage;
    if (current_free < ebuf->metrics.min_free_space) {
        ebuf->metrics.min_free_space = current_free;
    }

    /* Update current level */
    ebuf->metrics.current_level = ebuf->current_watermark;

    /* Update peak level */
    if (ebuf->current_watermark > ebuf->metrics.peak_level) {
        ebuf->metrics.peak_level = ebuf->current_watermark;
        ebuf->metrics.time_at_peak_level = time(NULL);
    }

    /* Update average fill ratio */
    double current_fill_ratio = (double)ebuf->metrics.current_usage / total_capacity;
    if (ebuf->metrics.avg_fill_ratio == 0.0) {
        ebuf->metrics.avg_fill_ratio = current_fill_ratio;
    } else {
        /* Exponential moving average with alpha = 0.1 */
        ebuf->metrics.avg_fill_ratio = 0.9 * ebuf->metrics.avg_fill_ratio + 0.1 * current_fill_ratio;
    }

    ebuf->metrics.last_activity = time(NULL);

    /* BACKPRESSURE.txt compliant dynamic resizing */
    if (ebuf->config.adaptive_sizing_enabled) {
        /* Check if resizing is needed (rate-limited to prevent thrashing) */
        time_t now = time(NULL);
        if (now - ebuf->last_resize_time > 30) {  /* Minimum 30 seconds between resizes */
            bool should_grow = false;
            bool should_shrink = false;

            /* Growth condition: consistent high usage or frequent overflows */
            if (current_fill_ratio > 0.85 || ebuf->consecutive_overflows >= 3) {
                should_grow = true;
            }
            /* Shrink condition: consistent low usage */
            else if (current_fill_ratio < 0.15 && ebuf->buffer_size > ebuf->config.min_buffer_size) {
                should_shrink = true;
            }

            if (should_grow || should_shrink) {
                size_t new_size = ebuf->buffer_size;

                if (should_grow) {
                    /* Grow by configured step size, but don't exceed maximum */
                    new_size = ebuf->buffer_size + ebuf->config.growth_step_size;
                    if (new_size > ebuf->config.max_buffer_size) {
                        new_size = ebuf->config.max_buffer_size;
                    }
                    MB_LOG_INFO("Growing buffer: %zu -> %zu bytes (fill_ratio: %.2f, overflows: %d)",
                                ebuf->buffer_size, new_size, current_fill_ratio, ebuf->consecutive_overflows);
                } else if (should_shrink) {
                    /* Shrink by configured step size, but don't go below minimum */
                    new_size = ebuf->buffer_size - ebuf->config.shrink_step_size;
                    if (new_size < ebuf->config.min_buffer_size) {
                        new_size = ebuf->config.min_buffer_size;
                    }
                    MB_LOG_INFO("Shrinking buffer: %zu -> %zu bytes (fill_ratio: %.2f)",
                                ebuf->buffer_size, new_size, current_fill_ratio);
                }

                /* Apply resize if different */
                if (new_size != ebuf->buffer_size) {
                    int ret = l3_resize_buffer(ebuf, new_size);
                    if (ret == L3_SUCCESS) {
                        ebuf->last_resize_time = now;
                        /* Reset overflow/underflow counters after successful resize */
                        ebuf->consecutive_overflows = 0;
                        ebuf->consecutive_underflows = 0;
                    }
                }
            }
        }
    }

    return L3_SUCCESS;
}

/**
 * Get comprehensive buffer statistics
 * @param ebuf Enhanced double buffer context
 * @param metrics Output structure for buffer metrics
 * @return SUCCESS on success, error code on failure
 */
int l3_get_buffer_metrics(l3_enhanced_double_buffer_t *ebuf, l3_buffer_metrics_t *metrics)
{
    if (!ebuf || !metrics) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&ebuf->mutex);

    /* Copy current metrics */
    *metrics = ebuf->metrics;

    /* Update current values */
    metrics->current_usage = ebuf->main_len + ebuf->sub_len;
    metrics->current_level = ebuf->current_watermark;

    pthread_mutex_unlock(&ebuf->mutex);

    return L3_SUCCESS;
}

/**
 * Initialize memory pool for fragmentation prevention
 * @param pool Memory pool to initialize
 * @param pool_size Total size of memory pool
 * @param block_size Size of individual blocks
 * @return SUCCESS on success, error code on failure
 */
int l3_memory_pool_init(l3_memory_pool_t *pool, size_t pool_size, size_t block_size)
{
    if (!pool || pool_size == 0 || block_size == 0) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(pool, 0, sizeof(l3_memory_pool_t));

    /* Allocate memory pool */
    pool->pool_memory = malloc(pool_size);
    if (!pool->pool_memory) {
        MB_LOG_ERROR("Failed to allocate memory pool of %zu bytes", pool_size);
        return L3_ERROR_MEMORY;
    }

    /* Initialize pool parameters */
    pool->pool_size = pool_size;
    pool->block_size = block_size;
    pool->total_blocks = pool_size / block_size;
    pool->free_blocks = pool->total_blocks;

    /* Initialize mutex */
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize memory pool mutex");
        free(pool->pool_memory);
        return L3_ERROR_THREAD;
    }

    /* Initially all blocks are free */
    pool->fragmentation_ratio = 0.0;
    pool->largest_free_block = pool_size;

    MB_LOG_DEBUG("Memory pool initialized: %zu bytes, %zu blocks of %zu bytes",
                pool_size, pool->total_blocks, block_size);

    return L3_SUCCESS;
}

/**
 * Allocate block from memory pool
 * @param pool Memory pool context
 * @return Pointer to allocated block, NULL if failed
 */
unsigned char *l3_memory_pool_alloc(l3_memory_pool_t *pool)
{
    if (!pool || !pool->pool_memory) {
        return NULL;
    }

    pthread_mutex_lock(&pool->pool_mutex);

    if (pool->free_blocks == 0) {
        MB_LOG_DEBUG("Memory pool exhausted - no free blocks");
        pthread_mutex_unlock(&pool->pool_mutex);
        return NULL;
    }

    /* Simple allocation - return next free block */
    /* In a real implementation, this would use a free list */
    unsigned char *block = pool->pool_memory +
                          (pool->allocated_blocks * pool->block_size);

    if (pool->allocated_blocks < pool->total_blocks) {
        pool->allocated_blocks++;
        pool->free_blocks--;
        pool->allocation_count++;

        /* Update fragmentation metrics */
        pool->fragmentation_ratio = (double)(pool->total_blocks - pool->free_blocks) / pool->total_blocks;
        pool->largest_free_block = pool->free_blocks * pool->block_size;
    } else {
        block = NULL;
    }

    pthread_mutex_unlock(&pool->pool_mutex);
    return block;
}

/**
 * Free block back to memory pool
 * @param pool Memory pool context
 * @param block Block to free (must be from this pool)
 * @return SUCCESS on success, error code on failure
 */
int l3_memory_pool_free(l3_memory_pool_t *pool, unsigned char *block)
{
    if (!pool || !block) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&pool->pool_mutex);

    /* Simple free - in a real implementation, this would add to free list */
    if (pool->allocated_blocks > 0) {
        pool->allocated_blocks--;
        pool->free_blocks++;
        pool->free_count++;

        /* Update fragmentation metrics */
        pool->fragmentation_ratio = (double)(pool->total_blocks - pool->free_blocks) / pool->total_blocks;
        pool->largest_free_block = pool->free_blocks * pool->block_size;
    }

    pthread_mutex_unlock(&pool->pool_mutex);
    return L3_SUCCESS;
}

/**
 * Cleanup memory pool and release all memory
 * @param pool Memory pool to cleanup
 */
void l3_memory_pool_cleanup(l3_memory_pool_t *pool)
{
    if (!pool) {
        return;
    }

    free(pool->pool_memory);
    pthread_mutex_destroy(&pool->pool_mutex);
    memset(pool, 0, sizeof(l3_memory_pool_t));

    MB_LOG_DEBUG("Memory pool cleaned up");
}

/* ========== Backpressure Management ========== */

bool l3_should_apply_backpressure(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return false;
    }

    /* Check if output buffer is getting full (high watermark) */
    size_t available = l3_double_buffer_available(&pipeline->buffers);
    bool buffer_high = available > L3_HIGH_WATERMARK;

    /* Check if backpressure has been active too long */
    if (pipeline->backpressure_active) {
        time_t now = time(NULL);
        if (now - pipeline->backpressure_start > L3_BACKPRESSURE_TIMEOUT_MS / 1000) {
            MB_LOG_WARNING("Pipeline %s: Backpressure timeout, forcing release", pipeline->name);
            return false;
        }
    }

    return buffer_high;
}

int l3_apply_backpressure(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (!pipeline->backpressure_active) {
        pipeline->backpressure_active = true;
        pipeline->backpressure_start = time(NULL);
        pipeline->state = L3_PIPELINE_STATE_BLOCKED;
        MB_LOG_INFO("Pipeline %s: Backpressure applied", pipeline->name);
    }

    return L3_SUCCESS;
}

int l3_release_backpressure(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (pipeline->backpressure_active) {
        pipeline->backpressure_active = false;
        pipeline->state = L3_PIPELINE_STATE_ACTIVE;
        MB_LOG_INFO("Pipeline %s: Backpressure released", pipeline->name);
    }

    return L3_SUCCESS;
}

/* ========== Half-duplex Control ========== */

int l3_switch_active_pipeline(l3_context_t *l3_ctx, l3_pipeline_direction_t new_active_pipeline)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (!l3_can_switch_pipeline(l3_ctx)) {
        MB_LOG_DEBUG("Pipeline switch not allowed at this time");
        return L3_ERROR_BUSY;
    }

    l3_ctx->active_pipeline = new_active_pipeline;
    l3_ctx->last_pipeline_switch = time(NULL);
    l3_ctx->total_pipeline_switches++;

    MB_LOG_DEBUG("Pipeline switch: %s (switch #%llu)",
                l3_get_pipeline_name(new_active_pipeline),
                (unsigned long long)l3_ctx->total_pipeline_switches);

    return L3_SUCCESS;
}

bool l3_can_switch_pipeline(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return false;
    }

    /* Don't switch if not in half-duplex mode */
    if (!l3_ctx->half_duplex_mode) {
        return false;
    }

    /* Check minimum time between switches */
    time_t now = time(NULL);
    if (now - l3_ctx->last_pipeline_switch < 1) {  /* At least 1 second between switches */
        return false;
    }

    /* Check if current pipeline is in the middle of critical operation */
    l3_pipeline_t *current = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                             &l3_ctx->pipeline_serial_to_telnet :
                             &l3_ctx->pipeline_telnet_to_serial;

    if (current->state == L3_PIPELINE_STATE_ERROR) {
        return false;
    }

    return true;
}

/* ========== Statistics and Monitoring ========== */

void l3_print_stats(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return;
    }

    time_t uptime = time(NULL) - l3_ctx->start_time;

    MB_LOG_INFO("=== Level 3 Statistics ===");
    MB_LOG_INFO("System Uptime: %ld seconds", (long)uptime);
    MB_LOG_INFO("Total Pipeline Switches: %llu", (unsigned long long)l3_ctx->total_pipeline_switches);
    MB_LOG_INFO("System Utilization: %.2f%%", l3_ctx->system_utilization_pct);
    MB_LOG_INFO("Half-duplex Mode: %s", l3_ctx->half_duplex_mode ? "Enabled" : "Disabled");
    MB_LOG_INFO("Level 1 Ready: %s, Level 2 Ready: %s",
                l3_ctx->level1_ready ? "Yes" : "No",
                l3_ctx->level2_ready ? "Yes" : "No");
    MB_LOG_INFO("Scheduling Rounds: %d", l3_ctx->round_robin_counter);

    /* Print individual pipeline statistics */
    l3_print_pipeline_stats(&l3_ctx->pipeline_serial_to_telnet);
    l3_print_pipeline_stats(&l3_ctx->pipeline_telnet_to_serial);

    MB_LOG_INFO("==========================");
}

void l3_print_pipeline_stats(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return;
    }

    MB_LOG_INFO("Pipeline %s:", pipeline->name);
    MB_LOG_INFO("  State: %s", l3_pipeline_state_to_string(pipeline->state));
    MB_LOG_INFO("  Bytes Processed: %llu", (unsigned long long)pipeline->total_bytes_processed);
    MB_LOG_INFO("  Bytes Dropped: %llu", (unsigned long long)pipeline->total_bytes_dropped);
    MB_LOG_INFO("  Buffer Switches: %llu", (unsigned long long)pipeline->pipeline_switches);
    MB_LOG_INFO("  Avg Processing Time: %.2f ms", pipeline->avg_processing_time_ms);
    MB_LOG_INFO("  Backpressure Active: %s", pipeline->backpressure_active ? "Yes" : "No");
    MB_LOG_INFO("  Main Buffer Available: %zu bytes", l3_double_buffer_available(&pipeline->buffers));
    MB_LOG_INFO("  Sub Buffer Free: %zu bytes", l3_double_buffer_free(&pipeline->buffers));
}

double l3_get_system_utilization(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return 0.0;
    }

    /* Calculate utilization based on processing time vs total time */
    time_t total_time = time(NULL) - l3_ctx->start_time;
    if (total_time == 0) {
        return 0.0;
    }

    /* Estimate utilization from pipeline activity */
    double total_processing_time = l3_ctx->pipeline_serial_to_telnet.avg_processing_time_ms +
                                  l3_ctx->pipeline_telnet_to_serial.avg_processing_time_ms;

    double utilization = (total_processing_time / (total_time * 1000.0)) * 100.0;
    if (utilization > 100.0) {
        utilization = 100.0;
    }

    l3_ctx->system_utilization_pct = utilization;
    return utilization;
}

/* ========== Thread Functions ========== */

void *l3_management_thread_func(void *arg)
{
    l3_context_t *l3_ctx = (l3_context_t *)arg;

    MB_LOG_INFO("Level 3 management thread started");

    while (l3_ctx->thread_running) {
        /* Process state machine - handles all transitions automatically */
        int ret = l3_process_state_machine(l3_ctx);
        if (ret != L3_SUCCESS) {
            MB_LOG_ERROR("State machine processing failed: %d", ret);
            /* Continue running even if state machine fails */
        }

        /* Only process data if in DATA_TRANSFER state */
        if (l3_ctx->system_state == L3_STATE_DATA_TRANSFER && l3_ctx->level3_active) {
            /* DEBUG: Log entry into data processing (only once at start) */
            static bool entered_data_transfer = false;
            if (!entered_data_transfer) {
                printf("[DEBUG-MGMT-THREAD] Entered DATA_TRANSFER processing loop\n");
                fflush(stdout);
                entered_data_transfer = true;
            }

            /* Use enhanced quantum-based scheduling instead of basic round-robin */
            int ret = l3_process_pipeline_with_quantum(l3_ctx, l3_ctx->active_pipeline);

            if (ret != L3_SUCCESS) {
                MB_LOG_WARNING("Quantum-based pipeline processing failed: %d", ret);
                /* Fall back to basic scheduling if quantum processing fails */
                l3_pipeline_direction_t next_pipeline = l3_schedule_next_pipeline(l3_ctx);

                /* Switch if needed */
                if (next_pipeline != l3_ctx->active_pipeline) {
                    l3_switch_active_pipeline(l3_ctx, next_pipeline);

                    /* Update fair queue weights when switching pipelines */
                    l3_update_fair_queue_weights(l3_ctx);
                }

                /* Process active pipeline */
                l3_pipeline_t *active = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                                       &l3_ctx->pipeline_serial_to_telnet :
                                       &l3_ctx->pipeline_telnet_to_serial;

                /* Check for backpressure */
                if (l3_should_apply_backpressure(active)) {
                    l3_apply_backpressure(active);
                    usleep(10000);  /* 10ms delay under backpressure */
                    continue;
                } else {
                    l3_release_backpressure(active);
                }

                /* Process data based on pipeline direction using helper functions */
                if (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) {
                    l3_process_serial_to_telnet_chunk(l3_ctx);
                } else {
                    l3_process_telnet_to_serial_chunk(l3_ctx);
                }
            }

            /* Periodically update scheduling statistics and fair queue weights */
            static int statistics_counter = 0;
            if (++statistics_counter >= 100) {  /* Every 100 iterations */
                l3_scheduling_stats_t stats;
                if (l3_get_scheduling_statistics(l3_ctx, &stats) == L3_SUCCESS) {
                    MB_LOG_DEBUG("Scheduling stats - utilization: %.2f%%, fairness: %.2f, cycles: %d",
                                stats.system_utilization * 100.0, stats.fairness_ratio,
                                stats.total_scheduling_cycles);
                }
                l3_update_fair_queue_weights(l3_ctx);

                /* Periodic latency boundary enforcement for continuous monitoring */
                l3_enforce_latency_boundaries(l3_ctx);

                statistics_counter = 0;
            }
        } else {
            /* Not in data transfer state - wait for state changes */
            if (l3_ctx->system_state == L3_STATE_INITIALIZING) {
                usleep(100000);  /* 100ms during initialization */
            } else if (l3_ctx->system_state == L3_STATE_READY) {
                usleep(500000);  /* 500ms in ready state (DCD waiting) */
            } else if (l3_ctx->system_state == L3_STATE_CONNECTING ||
                      l3_ctx->system_state == L3_STATE_NEGOTIATING) {
                usleep(200000);  /* 200ms during connection/negotiation */
            } else if (l3_ctx->system_state == L3_STATE_FLUSHING) {
                usleep(50000);   /* 50ms during flushing (more frequent checks) */
            } else if (l3_ctx->system_state == L3_STATE_SHUTTING_DOWN ||
                      l3_ctx->system_state == L3_STATE_TERMINATED) {
                usleep(1000000); /* 1s during shutdown/termination */
            } else {
                usleep(L3_FAIRNESS_TIME_SLICE_MS * 1000);  /* Default timeslice */
            }
        }

        /* Update system utilization */
        l3_get_system_utilization(l3_ctx);
    }

    MB_LOG_INFO("Level 3 management thread exiting");
    return NULL;
}

/* ========== Utility Functions ========== */

const char *l3_get_pipeline_name(l3_pipeline_direction_t direction)
{
    switch (direction) {
        case L3_PIPELINE_SERIAL_TO_TELNET:
            return "Serial→Telnet";
        case L3_PIPELINE_TELNET_TO_SERIAL:
            return "Telnet→Serial";
        default:
            return "Unknown";
    }
}

long long l3_get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

const char *l3_pipeline_state_to_string(l3_pipeline_state_t state)
{
    switch (state) {
        case L3_PIPELINE_STATE_IDLE:
            return "IDLE";
        case L3_PIPELINE_STATE_ACTIVE:
            return "ACTIVE";
        case L3_PIPELINE_STATE_BLOCKED:
            return "BLOCKED";
        case L3_PIPELINE_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

/* ========== Enhanced Scheduling Helper Functions ========== */

/**
 * Get direction name as string for logging
 * @param direction Direction enum
 * @return String representation of direction
 */
static const char *l3_get_direction_name(l3_pipeline_direction_t direction)
{
    switch (direction) {
        case L3_PIPELINE_SERIAL_TO_TELNET:
            return "Serial→Telnet";
        case L3_PIPELINE_TELNET_TO_SERIAL:
            return "Telnet→Serial";
        default:
            return "Unknown";
    }
}

/**
 * Process a chunk of data from serial to telnet
 * Implements line buffering based on Level 1 modem buffer logic:
 * 1. Accumulate data in line buffer
 * 2. Process complete lines through Hayes filter
 * 3. Forward filtered data to telnet pipeline
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_process_serial_to_telnet_chunk(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Static line buffer (persists across calls) */
    static unsigned char line_buffer[LINE_BUFFER_SIZE];
    static size_t line_len = 0;
    static time_t line_start_time = 0;

    /* Static multibyte buffer for character assembly */
    static unsigned char multibyte_buffer[6];  /* Max UTF-8 is 4 bytes, but safety margin */
    static size_t multibyte_len = 0;
    static int multibyte_expected = 0;
    static time_t multibyte_start_time = 0;

    /* Read from serial→telnet buffer (populated by modem thread) */
    unsigned char serial_buf[L3_MAX_BURST_SIZE];
    size_t serial_len = ts_cbuf_read(&l3_ctx->bridge->ts_serial_to_telnet_buf,
                                     serial_buf, sizeof(serial_buf));

    /* DEBUG: Log read result */
    static int read_counter = 0;
    if (++read_counter % 50 == 0 || serial_len > 0) {  /* Log periodically or when data found */
        /* Commented out to reduce log noise
        printf("[DEBUG-CHUNK-READ] Serial→Telnet buffer read: %zu bytes\n", serial_len);
        fflush(stdout);
        */
    }

    if (serial_len > 0) {
        time_t now = time(NULL);

        /* Check for line buffer timeout (20 seconds) */
        if (line_len > 0 && (now - line_start_time) > 20) {
            MB_LOG_DEBUG("Line buffer timeout - clearing old data");
            line_len = 0;
            memset(line_buffer, 0, sizeof(line_buffer));
        }

        /* Check for multibyte timeout (1 second) */
        if (multibyte_len > 0 && (now - multibyte_start_time) > 1) {
            MB_LOG_DEBUG("Multibyte timeout - echoing incomplete sequence: %zu bytes", multibyte_len);
            /* Echo incomplete multibyte as-is */
            l3_echo_to_modem(l3_ctx, multibyte_buffer, multibyte_len);

            /* Reset multibyte state */
            multibyte_len = 0;
            multibyte_expected = 0;
            memset(multibyte_buffer, 0, sizeof(multibyte_buffer));
        }

        /* Process input data byte by byte for line buffering and echo */
        for (size_t i = 0; i < serial_len; i++) {
            unsigned char c = serial_buf[i];

            /* Track start time for new line */
            if (line_len == 0) {
                line_start_time = now;
            }

            /* === Echo handling (immediate for single-byte, after assembly for multibyte) === */

            /* Check if we're in the middle of multibyte assembly */
            if (multibyte_len > 0) {
                /* Add to multibyte buffer */
                if (multibyte_len < sizeof(multibyte_buffer)) {
                    multibyte_buffer[multibyte_len++] = c;
                }

                /* Check if multibyte sequence is complete */
                if (l3_is_multibyte_complete(multibyte_buffer, multibyte_len, multibyte_expected)) {
                    /* Echo the complete multibyte character */
                    l3_echo_to_modem(l3_ctx, multibyte_buffer, multibyte_len);
                    MB_LOG_DEBUG("Echoed multibyte character: %d bytes", multibyte_len);

                    /* Reset multibyte buffer */
                    multibyte_len = 0;
                    multibyte_expected = 0;
                    memset(multibyte_buffer, 0, sizeof(multibyte_buffer));
                }
            } else if (l3_is_multibyte_start(c)) {
                /* Start of new multibyte sequence */
                multibyte_buffer[0] = c;
                multibyte_len = 1;
                multibyte_expected = l3_get_multibyte_length(c);
                multibyte_start_time = now;  /* Track when multibyte started */
                MB_LOG_DEBUG("Started multibyte sequence, expecting %d bytes", multibyte_expected);
            } else {
                /* Single-byte character - echo immediately */
                l3_echo_to_modem(l3_ctx, &c, 1);

                /* Note: CR/LF already echoed above as single bytes */
                /* No need for special newline handling here */
            }

            /* === Line buffer handling (for telnet transmission) === */

            /* Add character to line buffer */
            if (line_len < sizeof(line_buffer) - 1) {
                line_buffer[line_len++] = c;
                line_buffer[line_len] = '\0';
            }

            /* Check if we have a complete line */
            bool line_complete = false;
            bool buffer_full = false;

            if (c == '\r' || c == '\n') {
                line_complete = true;
                printf("[DEBUG-LINE] Line complete (CR/LF): %zu bytes\n", line_len);
            } else if (line_len >= sizeof(line_buffer) - 1) {
                buffer_full = true;
                printf("[DEBUG-LINE] Line buffer full: %zu bytes\n", line_len);
            }

            /* Process complete line or full buffer */
            if (line_complete || buffer_full) {
                /* Debug log the line content */
                printf("[DEBUG-LINE] Processing line: [%.*s]\n",
                       (int)(line_len > 50 ? 50 : line_len), line_buffer);
                fflush(stdout);

                /* Process through Hayes filter */
                unsigned char filtered_buf[L3_MAX_BURST_SIZE];
                size_t filtered_len = 0;

                /* Apply Hayes filter to the complete line */
                hayes_filter_context_t *hayes_ctx = &l3_ctx->pipeline_serial_to_telnet.filter_state.hayes_ctx;
                int filter_ret = l3_filter_hayes_commands(hayes_ctx,
                                                         line_buffer, line_len,
                                                         filtered_buf, sizeof(filtered_buf),
                                                         &filtered_len);

                /* DEBUG: Log filter result */
                printf("[DEBUG-FILTER] Hayes filter: in=%zu, out=%zu, ret=%d\n",
                       line_len, filtered_len, filter_ret);
                if (filtered_len > 0 && filtered_len <= 20) {
                    printf("[DEBUG-FILTER] Output: ");
                    for (size_t j = 0; j < filtered_len; j++) {
                        printf("%02x ", filtered_buf[j]);
                    }
                    printf("(%.*s)\n", (int)filtered_len, filtered_buf);
                }
                fflush(stdout);

                /* Send filtered data to telnet */
                if (filter_ret == L3_SUCCESS && filtered_len > 0) {
#ifdef ENABLE_LEVEL2
                    int send_ret = telnet_queue_write(&l3_ctx->bridge->telnet,
                                                     filtered_buf, filtered_len);
                    if (send_ret != SUCCESS) {
                        MB_LOG_WARNING("Failed to queue data to telnet: %d", send_ret);
                        printf("[ERROR] telnet_queue_write failed: %d\n", send_ret);
                        fflush(stdout);
                        /* Don't return error - continue processing */
                    } else {
                        printf("[DEBUG-TELNET-SEND] Queued %zu bytes to telnet buffer\n", filtered_len);
                        fflush(stdout);

                        /* Flush the write buffer to actually send data to telnet server */
                        int flush_ret = telnet_flush_writes(&l3_ctx->bridge->telnet);
                        if (flush_ret != SUCCESS) {
                            MB_LOG_WARNING("Failed to flush telnet write buffer: %d", flush_ret);
                            printf("[ERROR] telnet_flush_writes failed: %d\n", flush_ret);
                            fflush(stdout);
                        } else {
                            printf("[DEBUG-TELNET-FLUSH] Flushed telnet write buffer successfully\n");
                            fflush(stdout);
                            MB_LOG_DEBUG("Line processed and sent: %zu → %zu bytes to telnet",
                                       line_len, filtered_len);
                        }
                    }
#endif
                }

                /* Clear line buffer for next line */
                line_len = 0;
                memset(line_buffer, 0, sizeof(line_buffer));

                /* If buffer was full and not line complete,
                   start new line with current character */
                if (buffer_full && !line_complete) {
                    line_buffer[0] = c;
                    line_len = 1;
                    line_start_time = now;
                }
            }
        }

        /* Update pipeline statistics */
        l3_update_pipeline_stats(&l3_ctx->pipeline_serial_to_telnet,
                                serial_len, 0.0);
    }

    return L3_SUCCESS;  /* No data available is not an error */
}

/**
 * Process a chunk of data from telnet to serial
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_process_telnet_to_serial_chunk(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

#ifdef ENABLE_LEVEL2
    /* Read from telnet buffer */
    unsigned char telnet_buf[L3_MAX_BURST_SIZE];
    size_t telnet_len = ts_cbuf_read(&l3_ctx->bridge->ts_telnet_to_serial_buf,
                                    telnet_buf, sizeof(telnet_buf));

    if (telnet_len > 0) {
        /* Process through pipeline */
        unsigned char filtered_buf[L3_MAX_BURST_SIZE];
        size_t filtered_len;

        int ret = l3_pipeline_process(&l3_ctx->pipeline_telnet_to_serial,
                                     telnet_buf, telnet_len,
                                     filtered_buf, sizeof(filtered_buf), &filtered_len);

        if (ret == L3_SUCCESS && filtered_len > 0) {
            /* Write to serial port */
            ssize_t sent = serial_write(&l3_ctx->bridge->serial,
                                       filtered_buf, filtered_len);
            if (sent > 0) {
                MB_LOG_DEBUG("Processed telnet→serial chunk: %zu → %zd bytes", telnet_len, sent);
                return L3_SUCCESS;
            } else {
                MB_LOG_WARNING("Failed to write to serial port");
                return L3_ERROR_IO;
            }
        }
        return ret;
    }
#endif

    return L3_SUCCESS;  /* No data available is not an error */
}

/* ========== Internal Helper Functions ========== */

static void l3_update_pipeline_stats(l3_pipeline_t *pipeline, size_t bytes_processed, double processing_time)
{
    if (pipeline == NULL) {
        return;
    }

    pipeline->total_bytes_processed += bytes_processed;
    pipeline->bytes_in_timeslice += bytes_processed;

    /* Update moving average of processing time */
    if (pipeline->avg_processing_time_ms == 0.0) {
        pipeline->avg_processing_time_ms = processing_time;
    } else {
        /* Exponential moving average with alpha = 0.1 */
        pipeline->avg_processing_time_ms = 0.9 * pipeline->avg_processing_time_ms + 0.1 * processing_time;
    }

    pipeline->last_activity = time(NULL);
}

/* ========== Latency Bound Guarantee Functions ========== */

/**
 * Enforce latency boundaries for both pipeline directions
 * This is the main function for implementing "지연 상한 보장" (latency bound guarantee)
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
