/*
 * telnet_thread.h - Level 2 Telnet-only thread management
 *
 * Independent thread management for Level 2 telnet functionality.
 * Isolates telnet operations from other levels and provides
 * dedicated telnet server connection handling.
 */

#ifndef MODEMBRIDGE_TELNET_THREAD_H
#define MODEMBRIDGE_TELNET_THREAD_H

#ifdef ENABLE_LEVEL2

#include "telnet.h"
#include <pthread.h>
#include <stdbool.h>

/* Level 2 Telnet thread states */
typedef enum {
    TELNET_THREAD_STOPPED,
    TELNET_THREAD_STARTING,
    TELNET_THREAD_RUNNING,
    TELNET_THREAD_STOPPING,
    TELNET_THREAD_ERROR
} telnet_thread_state_t;

/* Level 2 Telnet thread context */
typedef struct {
    /* Thread management */
    pthread_t thread_id;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    telnet_thread_state_t state;

    /* Telnet connection */
    telnet_t telnet;
    char host[256];
    int port;

    /* Thread control */
    bool should_stop;
    bool running;

    /* Error handling */
    int last_error;
    char error_msg[256];

    /* Statistics */
    uint64_t bytes_sent;
    uint64_t bytes_received;
    time_t start_time;
    time_t last_activity;

    /* Configuration */
    int connection_timeout;
    int reconnect_interval;
    bool auto_reconnect;

    /* Data buffers (independent from bridge) */
    unsigned char input_buffer[8192];
    unsigned char output_buffer[8192];
    size_t input_len;
    size_t output_len;

    /* Data callbacks (Level 2 only interface) */
    void (*data_received_callback)(const unsigned char *data, size_t len, void *context);
    void (*connection_changed_callback)(bool connected, void *context);
    void (*error_callback)(int error_code, const char *message, void *context);
    void *callback_context;

} telnet_thread_ctx_t;

/* Function prototypes */

/**
 * Initialize Level 2 telnet thread context
 * @param ctx Thread context to initialize
 * @param host Telnet server host
 * @param port Telnet server port
 * @return SUCCESS on success, error code on failure
 */
int telnet_thread_init(telnet_thread_ctx_t *ctx, const char *host, int port);

/**
 * Destroy Level 2 telnet thread context
 * @param ctx Thread context
 */
void telnet_thread_destroy(telnet_thread_ctx_t *ctx);

/**
 * Start Level 2 telnet thread
 * @param ctx Thread context
 * @return SUCCESS on success, error code on failure
 */
int telnet_thread_start(telnet_thread_ctx_t *ctx);

/**
 * Stop Level 2 telnet thread
 * @param ctx Thread context
 * @return SUCCESS on success, error code on failure
 */
int telnet_thread_stop(telnet_thread_ctx_t *ctx);

/**
 * Check if telnet thread is running
 * @param ctx Thread context
 * @return true if running, false otherwise
 */
bool telnet_thread_is_running(telnet_thread_ctx_t *ctx);

/**
 * Get telnet thread state
 * @param ctx Thread context
 * @return Current thread state
 */
telnet_thread_state_t telnet_thread_get_state(telnet_thread_ctx_t *ctx);

/**
 * Send data through Level 2 telnet thread
 * @param ctx Thread context
 * @param data Data to send
 * @param len Data length
 * @return SUCCESS on success, error code on failure
 */
int telnet_thread_send(telnet_thread_ctx_t *ctx, const void *data, size_t len);

/**
 * Get thread statistics
 * @param ctx Thread context
 * @param bytes_sent Pointer to store bytes sent (can be NULL)
 * @param bytes_received Pointer to store bytes received (can be NULL)
 * @param uptime Pointer to store uptime in seconds (can be NULL)
 */
void telnet_thread_get_stats(telnet_thread_ctx_t *ctx, uint64_t *bytes_sent,
                           uint64_t *bytes_received, time_t *uptime);

/**
 * Set thread configuration
 * @param ctx Thread context
 * @param connection_timeout Connection timeout in seconds
 * @param reconnect_interval Reconnection interval in seconds
 * @param auto_reconnect Enable automatic reconnection
 */
void telnet_thread_set_config(telnet_thread_ctx_t *ctx, int connection_timeout,
                             int reconnect_interval, bool auto_reconnect);

/**
 * Set data received callback
 * @param ctx Thread context
 * @param callback Callback function
 * @param context User context for callback
 */
void telnet_thread_set_data_callback(telnet_thread_ctx_t *ctx,
                                    void (*callback)(const unsigned char *, size_t, void *),
                                    void *context);

/**
 * Set connection state change callback
 * @param ctx Thread context
 * @param callback Callback function
 * @param context User context for callback
 */
void telnet_thread_set_connection_callback(telnet_thread_ctx_t *ctx,
                                          void (*callback)(bool, void *),
                                          void *context);

/**
 * Set error callback
 * @param ctx Thread context
 * @param callback Callback function
 * @param context User context for callback
 */
void telnet_thread_set_error_callback(telnet_thread_ctx_t *ctx,
                                     void (*callback)(int, const char *, void *),
                                     void *context);

/**
 * Get last error message
 * @param ctx Thread context
 * @return Error message string
 */
const char *telnet_thread_get_error(telnet_thread_ctx_t *ctx);

#endif /* ENABLE_LEVEL2 */

#endif /* MODEMBRIDGE_TELNET_THREAD_H */