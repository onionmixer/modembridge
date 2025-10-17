/*
 * telnet_thread.c - Level 2 Telnet-only thread implementation
 *
 * Independent thread implementation for Level 2 telnet functionality.
 * Provides isolated telnet server connection management without
 * dependencies on other levels.
 */

#include "telnet_thread.h"
#include "common.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>

/**
 * Level 2 telnet thread main function
 */
static void *telnet_thread_func(void *arg)
{
    telnet_thread_ctx_t *ctx = (telnet_thread_ctx_t *)arg;
    unsigned char clean_buffer[4096];

    MB_LOG_INFO("Level 2 telnet thread started for %s:%d", ctx->host, ctx->port);

    /* Initialize telnet connection */
    telnet_init(&ctx->telnet);
    telnet_set_keepalive(&ctx->telnet, true, 30, ctx->connection_timeout);
    telnet_set_error_handling(&ctx->telnet, ctx->auto_reconnect, 3, ctx->reconnect_interval);

    /* Set running flag */
    pthread_mutex_lock(&ctx->state_mutex);
    ctx->running = true;
    ctx->state = TELNET_THREAD_RUNNING;
    pthread_cond_broadcast(&ctx->state_cond);
    pthread_mutex_unlock(&ctx->state_mutex);

    /* Connection state change callback */
    if (ctx->connection_changed_callback) {
        ctx->connection_changed_callback(false, ctx->callback_context);
    }

    /* Main thread loop */
    while (!ctx->should_stop) {
        /* Connect if not connected */
        bool is_connected = telnet_is_connected(&ctx->telnet);
        printf("[TELNET-THREAD-DEBUG] Loop iteration: is_connected=%d\n", is_connected);
        fflush(stdout);

        if (!is_connected) {
            printf("[TELNET-THREAD-DEBUG] Not connected, checking if should reconnect...\n");
            fflush(stdout);

            if (telnet_should_reconnect(&ctx->telnet)) {
                printf("[TELNET-THREAD-DEBUG] telnet_should_reconnect() returned TRUE - attempting connection\n");
                fflush(stdout);
                MB_LOG_INFO("Level 2 telnet attempting reconnection to %s:%d", ctx->host, ctx->port);

                int result = telnet_connect(&ctx->telnet, ctx->host, ctx->port);
                printf("[TELNET-THREAD-DEBUG] telnet_connect() returned: %d\n", result);
                fflush(stdout);

                if (result == SUCCESS) {
                    MB_LOG_INFO("Level 2 telnet connected to %s:%d", ctx->host, ctx->port);
                    telnet_reset_error_state(&ctx->telnet);

                    if (ctx->connection_changed_callback) {
                        ctx->connection_changed_callback(true, ctx->callback_context);
                    }
                } else {
                    MB_LOG_ERROR("Level 2 telnet connection failed: %d", result);
                    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                            "Connection failed: %d", result);
                    ctx->last_error = result;

                    if (ctx->error_callback) {
                        ctx->error_callback(result, ctx->error_msg, ctx->callback_context);
                    }
                }
            } else {
                printf("[TELNET-THREAD-DEBUG] telnet_should_reconnect() returned FALSE - sleeping 1 second\n");
                fflush(stdout);
            }

            /* Sleep before retry */
            usleep(1000000); /* 1 second */
            continue;
        }

        /* Process epoll events */
        int result = telnet_process_events(&ctx->telnet, 100);
        if (result != SUCCESS) {
            MB_LOG_ERROR("Level 2 telnet event processing failed: %d", result);
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                    "Event processing failed: %d", result);
            ctx->last_error = result;
            telnet_disconnect(&ctx->telnet);
            continue;
        }

        /* Check connection health */
        result = telnet_check_connection_health(&ctx->telnet);
        if (result != SUCCESS) {
            MB_LOG_WARNING("Level 2 telnet health check failed: %d", result);
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                    "Health check failed: %d", result);
            ctx->last_error = result;
            telnet_disconnect(&ctx->telnet);
            continue;
        }

        /* Handle read events */
        if (telnet_can_read(&ctx->telnet)) {
            size_t output_len = 0;
            result = telnet_process_reads(&ctx->telnet, clean_buffer,
                                        sizeof(clean_buffer), &output_len);

            if (result == SUCCESS && output_len > 0) {
                /* Update statistics */
                ctx->bytes_received += output_len;
                ctx->last_activity = time(NULL);

                /* Data received callback */
                if (ctx->data_received_callback) {
                    ctx->data_received_callback(clean_buffer, output_len,
                                            ctx->callback_context);
                }
            } else if (result != SUCCESS) {
                MB_LOG_ERROR("Level 2 telnet read failed: %d", result);
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                        "Read failed: %d", result);
                ctx->last_error = result;

                /* Error callback */
                if (ctx->error_callback) {
                    ctx->error_callback(result, ctx->error_msg, ctx->callback_context);
                }
            }
        }

        /* Handle write events */
        if (telnet_can_write(&ctx->telnet)) {
            result = telnet_flush_writes(&ctx->telnet);
            if (result != SUCCESS) {
                MB_LOG_ERROR("Level 2 telnet write failed: %d", result);
                snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                        "Write failed: %d", result);
                ctx->last_error = result;
            }
        }

        /* Check for errors */
        if (telnet_has_error(&ctx->telnet)) {
            MB_LOG_WARNING("Level 2 telnet error detected, disconnecting");
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                    "Telnet error detected");
            ctx->last_error = ERROR_CONNECTION;

            /* Connection state change callback */
            if (ctx->connection_changed_callback) {
                ctx->connection_changed_callback(false, ctx->callback_context);
            }

            telnet_disconnect(&ctx->telnet);
        }

        /* Small delay to prevent busy waiting */
        usleep(10000); /* 10ms */
    }

    /* Cleanup */
    telnet_disconnect(&ctx->telnet);

    /* Update state */
    pthread_mutex_lock(&ctx->state_mutex);
    ctx->running = false;
    ctx->state = TELNET_THREAD_STOPPED;
    pthread_cond_broadcast(&ctx->state_cond);
    pthread_mutex_unlock(&ctx->state_mutex);

    MB_LOG_INFO("Level 2 telnet thread stopped");
    return NULL;
}

/**
 * Initialize Level 2 telnet thread context
 */
int telnet_thread_init(telnet_thread_ctx_t *ctx, const char *host, int port)
{
    if (ctx == NULL || host == NULL) {
        return ERROR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(telnet_thread_ctx_t));

    /* Copy connection info */
    SAFE_STRNCPY(ctx->host, host, sizeof(ctx->host));
    ctx->port = port;

    /* Initialize defaults */
    ctx->connection_timeout = 30;
    ctx->reconnect_interval = 10;
    ctx->auto_reconnect = true;
    ctx->state = TELNET_THREAD_STOPPED;

    /* Initialize mutex and condition variable */
    if (pthread_mutex_init(&ctx->state_mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize state mutex");
        return ERROR_SYSTEM;
    }

    if (pthread_cond_init(&ctx->state_cond, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize state condition");
        pthread_mutex_destroy(&ctx->state_mutex);
        return ERROR_SYSTEM;
    }

    MB_LOG_DEBUG("Level 2 telnet thread context initialized for %s:%d", host, port);
    return SUCCESS;
}

/**
 * Destroy Level 2 telnet thread context
 */
void telnet_thread_destroy(telnet_thread_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* Stop thread if running */
    if (ctx->running) {
        telnet_thread_stop(ctx);
    }

    /* Destroy synchronization objects */
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_cond_destroy(&ctx->state_cond);

    MB_LOG_DEBUG("Level 2 telnet thread context destroyed");
}

/**
 * Start Level 2 telnet thread
 */
int telnet_thread_start(telnet_thread_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    pthread_mutex_lock(&ctx->state_mutex);

    if (ctx->running) {
        pthread_mutex_unlock(&ctx->state_mutex);
        MB_LOG_WARNING("Level 2 telnet thread already running");
        return SUCCESS;
    }

    /* Set starting state */
    ctx->should_stop = false;
    ctx->state = TELNET_THREAD_STARTING;
    ctx->start_time = time(NULL);

    /* Create thread */
    if (pthread_create(&ctx->thread_id, NULL, telnet_thread_func, ctx) != 0) {
        MB_LOG_ERROR("Failed to create Level 2 telnet thread: %s", strerror(errno));
        ctx->state = TELNET_THREAD_ERROR;
        pthread_mutex_unlock(&ctx->state_mutex);
        return ERROR_SYSTEM;
    }

    /* Wait for thread to start */
    while (ctx->state == TELNET_THREAD_STARTING) {
        pthread_cond_wait(&ctx->state_cond, &ctx->state_mutex);
    }

    pthread_mutex_unlock(&ctx->state_mutex);

    if (ctx->state != TELNET_THREAD_RUNNING) {
        MB_LOG_ERROR("Level 2 telnet thread failed to start");
        return ERROR_SYSTEM;
    }

    MB_LOG_INFO("Level 2 telnet thread started successfully");
    return SUCCESS;
}

/**
 * Stop Level 2 telnet thread
 */
int telnet_thread_stop(telnet_thread_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    pthread_mutex_lock(&ctx->state_mutex);

    if (!ctx->running) {
        pthread_mutex_unlock(&ctx->state_mutex);
        return SUCCESS;
    }

    /* Signal thread to stop */
    ctx->should_stop = true;
    ctx->state = TELNET_THREAD_STOPPING;
    pthread_mutex_unlock(&ctx->state_mutex);

    /* Wait for thread to stop */
    pthread_join(ctx->thread_id, NULL);

    MB_LOG_INFO("Level 2 telnet thread stopped");
    return SUCCESS;
}

/**
 * Check if telnet thread is running
 */
bool telnet_thread_is_running(telnet_thread_ctx_t *ctx)
{
    bool running = false;

    if (ctx != NULL) {
        pthread_mutex_lock(&ctx->state_mutex);
        running = ctx->running;
        pthread_mutex_unlock(&ctx->state_mutex);
    }

    return running;
}

/**
 * Get telnet thread state
 */
telnet_thread_state_t telnet_thread_get_state(telnet_thread_ctx_t *ctx)
{
    telnet_thread_state_t state = TELNET_THREAD_STOPPED;

    if (ctx != NULL) {
        pthread_mutex_lock(&ctx->state_mutex);
        state = ctx->state;
        pthread_mutex_unlock(&ctx->state_mutex);
    }

    return state;
}

/**
 * Send data through Level 2 telnet thread
 */
int telnet_thread_send(telnet_thread_ctx_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL || data == NULL || len == 0) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->running) {
        return ERROR_CONNECTION;
    }

    /* Queue data for sending */
    int result = telnet_queue_write(&ctx->telnet, data, len);
    if (result == SUCCESS) {
        ctx->bytes_sent += len;
        ctx->last_activity = time(NULL);
    }

    return result;
}

/**
 * Get thread statistics
 */
void telnet_thread_get_stats(telnet_thread_ctx_t *ctx, uint64_t *bytes_sent,
                           uint64_t *bytes_received, time_t *uptime)
{
    if (ctx == NULL) {
        return;
    }

    if (bytes_sent) *bytes_sent = ctx->bytes_sent;
    if (bytes_received) *bytes_received = ctx->bytes_received;
    if (uptime) *uptime = ctx->running ? (time(NULL) - ctx->start_time) : 0;
}

/**
 * Set thread configuration
 */
void telnet_thread_set_config(telnet_thread_ctx_t *ctx, int connection_timeout,
                             int reconnect_interval, bool auto_reconnect)
{
    if (ctx == NULL) {
        return;
    }

    ctx->connection_timeout = connection_timeout > 0 ? connection_timeout : 30;
    ctx->reconnect_interval = reconnect_interval > 0 ? reconnect_interval : 10;
    ctx->auto_reconnect = auto_reconnect;

    MB_LOG_DEBUG("Level 2 telnet thread config: timeout=%ds, reconnect=%ds, auto_reconnect=%s",
                ctx->connection_timeout, ctx->reconnect_interval,
                auto_reconnect ? "enabled" : "disabled");
}

/**
 * Set data received callback
 */
void telnet_thread_set_data_callback(telnet_thread_ctx_t *ctx,
                                    void (*callback)(const unsigned char *, size_t, void *),
                                    void *context)
{
    if (ctx != NULL) {
        ctx->data_received_callback = callback;
        ctx->callback_context = context;
    }
}

/**
 * Set connection state change callback
 */
void telnet_thread_set_connection_callback(telnet_thread_ctx_t *ctx,
                                          void (*callback)(bool, void *),
                                          void *context)
{
    if (ctx != NULL) {
        ctx->connection_changed_callback = callback;
        ctx->callback_context = context;
    }
}

/**
 * Set error callback
 */
void telnet_thread_set_error_callback(telnet_thread_ctx_t *ctx,
                                     void (*callback)(int, const char *, void *),
                                     void *context)
{
    if (ctx != NULL) {
        ctx->error_callback = callback;
        ctx->callback_context = context;
    }
}

/**
 * Get last error message
 */
const char *telnet_thread_get_error(telnet_thread_ctx_t *ctx)
{
    return (ctx != NULL) ? ctx->error_msg : "Invalid context";
}