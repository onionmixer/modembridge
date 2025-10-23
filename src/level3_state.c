/*
 * level3_state.c - State machine functions for Level 3 Pipeline Management
 *
 * This file contains state machine functions extracted from level3.c
 * for better modularity and maintainability.
 */

#include "level3.h"         /* Must be included first for l3_context_t definition */
#include "level3_state.h"   /* State machine function declarations */
#include "level3_buffer.h"  /* Buffer management functions */
#include "common.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* Include telnet.h conditionally */
#ifdef ENABLE_LEVEL2
#include "telnet.h"
#endif

/* Global state for telnet connection attempts in CONNECTING state */
bool g_level3_connection_attempted = false;
time_t g_level3_last_attempt = 0;
bool g_level3_transition_logged = false;

/* ========== State Machine Core Functions ========== */

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

    l3_system_state_t old_state = l3_ctx->system_state;

    /* Check if transition is valid */
    if (!l3_is_valid_state_transition(old_state, new_state)) {
        MB_LOG_WARNING("Invalid state transition: %s -> %s",
                    l3_system_state_to_string(l3_ctx->system_state),
                    l3_system_state_to_string(new_state));
        pthread_mutex_unlock(&l3_ctx->state_mutex);
        return L3_ERROR_INVALID_STATE;
    }

    /* Update state */
    l3_ctx->system_state = new_state;
    l3_ctx->state_change_time = time(NULL);
    l3_ctx->state_timeout = timeout_seconds;

    /* Log state transition */
    MB_LOG_INFO("State transition: %s -> %s (timeout: %ds)",
                l3_system_state_to_string(old_state),
                l3_system_state_to_string(new_state),
                timeout_seconds);

    /* Signal any waiting threads */
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
#ifdef ENABLE_LEVEL2
            l3_ctx->level2_ready = telnet_is_connected(&l3_ctx->bridge->telnet);
#endif

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
                g_level3_connection_attempted = false;

                /* Reset transition log flag for next connection */
                g_level3_transition_logged = false;

                pthread_mutex_unlock(&l3_ctx->state_mutex);
                ret = l3_set_system_state(l3_ctx, L3_STATE_CONNECTING, LEVEL3_CONNECT_TIMEOUT);
                pthread_mutex_lock(&l3_ctx->state_mutex);
            }
            break;

        case L3_STATE_CONNECTING:
#ifdef ENABLE_LEVEL2
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
#else
            /* Without Level 2, go directly to data transfer */
            l3_ctx->negotiation_complete = true;
            l3_ctx->level3_active = true;
            pthread_mutex_unlock(&l3_ctx->state_mutex);
            ret = l3_set_system_state(l3_ctx, L3_STATE_DATA_TRANSFER, 0);
            pthread_mutex_lock(&l3_ctx->state_mutex);
#endif
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
            {
                bool serial_empty = (l3_double_buffer_available(&l3_ctx->pipeline_serial_to_telnet.buffers) == 0);
                bool telnet_empty = (l3_double_buffer_available(&l3_ctx->pipeline_telnet_to_serial.buffers) == 0);

                if (serial_empty && telnet_empty) {
                    MB_LOG_INFO("Buffers flushed - shutting down");
                    pthread_mutex_unlock(&l3_ctx->state_mutex);
                    ret = l3_set_system_state(l3_ctx, L3_STATE_SHUTTING_DOWN, 5);
                    pthread_mutex_lock(&l3_ctx->state_mutex);
                }
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

/* ========== State Timeout Functions ========== */

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

/* ========== DCD State Functions ========== */

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
 * Handle DCD rising edge event - activates pipeline when ready
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_on_dcd_rising(l3_context_t *l3_ctx)
{
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