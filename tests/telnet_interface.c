/*
 * telnet_interface.c - Level 2 Telnet-only data interface implementation
 *
 * Isolated data interface implementation for Level 2 telnet functionality.
 * Provides clean API boundaries and minimal coupling with other levels.
 */

#include "telnet_interface.h"
#include "common.h"
#include <string.h>
#include <time.h>

/**
 * Internal callback functions for thread events
 */
static void on_thread_connection_changed(bool connected, void *context)
{
    telnet_iface_t *iface = (telnet_iface_t *)context;
    telnet_iface_state_t old_state = iface->state;

    if (connected) {
        iface->state = TELNET_IFACE_CONNECTED;
        iface->connection_time = time(NULL);
        MB_LOG_INFO("Level 2 telnet interface connected to %s:%d", iface->server_host, iface->server_port);
    } else {
        iface->state = TELNET_IFACE_DISCONNECTED;
        iface->connection_time = 0;
        MB_LOG_INFO("Level 2 telnet interface disconnected from %s:%d", iface->server_host, iface->server_port);
    }

    /* Clear error on state change */
    if (connected) {
        iface->last_error_code = 0;
        iface->last_error_message[0] = '\0';
    }

    /* Update last activity */
    iface->last_activity = time(NULL);

    /* Call user callback if set */
    if (iface->on_state_changed && old_state != iface->state) {
        iface->on_state_changed(old_state, iface->state, iface->callback_context);
    }
}

static void on_thread_data_received(const unsigned char *data, size_t len, void *context)
{
    telnet_iface_t *iface = (telnet_iface_t *)context;

    /* Update statistics */
    iface->total_bytes_received += len;
    iface->last_activity = time(NULL);

    /* Call user callback if set */
    if (iface->on_data_received) {
        iface->on_data_received(data, len, iface->callback_context);
    }
}

static void on_thread_error(int error_code, const char *message, void *context)
{
    telnet_iface_t *iface = (telnet_iface_t *)context;

    /* Update error information */
    iface->last_error_code = error_code;
    SAFE_STRNCPY(iface->last_error_message, message ? message : "Unknown error",
                sizeof(iface->last_error_message));

    /* Update state if it's a connection error */
    if (error_code == ERROR_CONNECTION || error_code == ERROR_IO) {
        telnet_iface_state_t old_state = iface->state;
        iface->state = TELNET_IFACE_ERROR;

        /* Call user callback if state changed */
        if (iface->on_state_changed && old_state != iface->state) {
            iface->on_state_changed(old_state, iface->state, iface->callback_context);
        }
    }

    /* Call user error callback */
    if (iface->on_error) {
        iface->on_error(error_code, iface->last_error_message, iface->callback_context);
    }
}

/**
 * Initialize Level 2 telnet interface
 */
int telnet_iface_init(telnet_iface_t *iface, const char *server_host, int server_port)
{
    if (iface == NULL || server_host == NULL) {
        return ERROR_INVALID_ARG;
    }

    memset(iface, 0, sizeof(telnet_iface_t));

    /* Copy server configuration */
    SAFE_STRNCPY(iface->server_host, server_host, sizeof(iface->server_host));
    iface->server_port = server_port;

    /* Set initial state */
    iface->state = TELNET_IFACE_DISCONNECTED;

    /* Initialize thread context */
    int result = telnet_thread_init(&iface->thread_ctx, server_host, server_port);
    if (result != SUCCESS) {
        MB_LOG_ERROR("Failed to initialize Level 2 telnet thread: %d", result);
        return result;
    }

    /* Set internal callbacks */
    telnet_thread_set_connection_callback(&iface->thread_ctx, on_thread_connection_changed, iface);
    telnet_thread_set_data_callback(&iface->thread_ctx, on_thread_data_received, iface);
    telnet_thread_set_error_callback(&iface->thread_ctx, on_thread_error, iface);

    MB_LOG_INFO("Level 2 telnet interface initialized for %s:%d", server_host, server_port);
    return SUCCESS;
}

/**
 * Destroy Level 2 telnet interface
 */
void telnet_iface_destroy(telnet_iface_t *iface)
{
    if (iface == NULL) {
        return;
    }

    /* Stop interface if running */
    if (telnet_iface_is_connected(iface)) {
        telnet_iface_stop(iface);
    }

    /* Destroy thread context */
    telnet_thread_destroy(&iface->thread_ctx);

    /* Clear all data */
    memset(iface, 0, sizeof(telnet_iface_t));

    MB_LOG_INFO("Level 2 telnet interface destroyed");
}

/**
 * Start Level 2 telnet interface
 */
int telnet_iface_start(telnet_iface_t *iface)
{
    if (iface == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (iface->state == TELNET_IFACE_CONNECTED) {
        MB_LOG_WARNING("Level 2 telnet interface already connected");
        return SUCCESS;
    }

    MB_LOG_INFO("Starting Level 2 telnet interface for %s:%d", iface->server_host, iface->server_port);

    /* Set connecting state */
    telnet_iface_state_t old_state = iface->state;
    iface->state = TELNET_IFACE_CONNECTING;

    /* Call state change callback */
    if (iface->on_state_changed && old_state != iface->state) {
        iface->on_state_changed(old_state, iface->state, iface->callback_context);
    }

    /* Start thread */
    int result = telnet_thread_start(&iface->thread_ctx);
    if (result != SUCCESS) {
        iface->state = TELNET_IFACE_ERROR;
        snprintf(iface->last_error_message, sizeof(iface->last_error_message),
                "Failed to start thread: %d", result);
        iface->last_error_code = result;

        /* Call error callback */
        if (iface->on_error) {
            iface->on_error(result, iface->last_error_message, iface->callback_context);
        }

        MB_LOG_ERROR("Failed to start Level 2 telnet interface: %d", result);
        return result;
    }

    MB_LOG_INFO("Level 2 telnet interface started successfully");
    return SUCCESS;
}

/**
 * Stop Level 2 telnet interface
 */
int telnet_iface_stop(telnet_iface_t *iface)
{
    if (iface == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (iface->state == TELNET_IFACE_DISCONNECTED) {
        return SUCCESS;
    }

    MB_LOG_INFO("Stopping Level 2 telnet interface");

    /* Stop thread */
    int result = telnet_thread_stop(&iface->thread_ctx);
    if (result != SUCCESS) {
        MB_LOG_ERROR("Failed to stop Level 2 telnet thread: %d", result);
        return result;
    }

    /* Update state */
    telnet_iface_state_t old_state = iface->state;
    iface->state = TELNET_IFACE_DISCONNECTED;
    iface->connection_time = 0;

    /* Call state change callback */
    if (iface->on_state_changed && old_state != iface->state) {
        iface->on_state_changed(old_state, iface->state, iface->callback_context);
    }

    MB_LOG_INFO("Level 2 telnet interface stopped");
    return SUCCESS;
}

/**
 * Send data through Level 2 telnet interface
 */
int telnet_iface_send(telnet_iface_t *iface, const void *data, size_t len)
{
    if (iface == NULL || data == NULL || len == 0) {
        return ERROR_INVALID_ARG;
    }

    if (iface->state != TELNET_IFACE_CONNECTED) {
        return ERROR_CONNECTION;
    }

    /* Send through thread */
    int result = telnet_thread_send(&iface->thread_ctx, data, len);
    if (result == SUCCESS) {
        iface->total_bytes_sent += len;
        iface->last_activity = time(NULL);
    }

    return result;
}

/**
 * Send text string through Level 2 telnet interface
 */
int telnet_iface_send_string(telnet_iface_t *iface, const char *text)
{
    if (text == NULL) {
        return ERROR_INVALID_ARG;
    }

    return telnet_iface_send(iface, text, strlen(text));
}

/**
 * Get current interface state
 */
telnet_iface_state_t telnet_iface_get_state(telnet_iface_t *iface)
{
    return (iface != NULL) ? iface->state : TELNET_IFACE_ERROR;
}

/**
 * Check if interface is connected and ready
 */
bool telnet_iface_is_connected(telnet_iface_t *iface)
{
    return (iface != NULL) && (iface->state == TELNET_IFACE_CONNECTED);
}

/**
 * Get last error information
 */
bool telnet_iface_get_last_error(telnet_iface_t *iface, int *error_code,
                                char *error_message, size_t message_size)
{
    if (iface == NULL) {
        return false;
    }

    bool has_error = (iface->last_error_code != 0);

    if (has_error) {
        if (error_code) *error_code = iface->last_error_code;
        if (error_message && message_size > 0) {
            SAFE_STRNCPY(error_message, iface->last_error_message, message_size);
        }
    }

    return has_error;
}

/**
 * Get interface statistics
 */
void telnet_iface_get_statistics(telnet_iface_t *iface, uint64_t *bytes_sent,
                                uint64_t *bytes_received, time_t *connection_time,
                                time_t *last_activity)
{
    if (iface == NULL) {
        return;
    }

    if (bytes_sent) *bytes_sent = iface->total_bytes_sent;
    if (bytes_received) *bytes_received = iface->total_bytes_received;
    if (connection_time) {
        *connection_time = (iface->state == TELNET_IFACE_CONNECTED && iface->connection_time > 0) ?
                          (time(NULL) - iface->connection_time) : 0;
    }
    if (last_activity) *last_activity = iface->last_activity;
}

/**
 * Set interface configuration
 */
void telnet_iface_set_config(telnet_iface_t *iface, int connection_timeout,
                            int reconnect_interval, bool auto_reconnect)
{
    if (iface == NULL) {
        return;
    }

    telnet_thread_set_config(&iface->thread_ctx, connection_timeout,
                            reconnect_interval, auto_reconnect);

    MB_LOG_DEBUG("Level 2 telnet interface config updated");
}

/**
 * Set state change callback
 */
void telnet_iface_set_state_callback(telnet_iface_t *iface,
                                     void (*callback)(telnet_iface_state_t, telnet_iface_state_t, void *),
                                     void *context)
{
    if (iface != NULL) {
        iface->on_state_changed = callback;
        iface->callback_context = context;
    }
}

/**
 * Set data received callback
 */
void telnet_iface_set_data_callback(telnet_iface_t *iface,
                                   void (*callback)(const unsigned char *, size_t, void *),
                                   void *context)
{
    if (iface != NULL) {
        iface->on_data_received = callback;
        iface->callback_context = context;
    }
}

/**
 * Set error callback
 */
void telnet_iface_set_error_callback(telnet_iface_t *iface,
                                    void (*callback)(int, const char *, void *),
                                    void *context)
{
    if (iface != NULL) {
        iface->on_error = callback;
        iface->callback_context = context;
    }
}

/**
 * Get state name as string
 */
const char *telnet_iface_state_to_string(telnet_iface_state_t state)
{
    switch (state) {
        case TELNET_IFACE_DISCONNECTED: return "DISCONNECTED";
        case TELNET_IFACE_CONNECTING: return "CONNECTING";
        case TELNET_IFACE_CONNECTED: return "CONNECTED";
        case TELNET_IFACE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

/**
 * Force reconnection attempt
 */
int telnet_iface_force_reconnect(telnet_iface_t *iface)
{
    if (iface == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (telnet_iface_is_connected(iface)) {
        MB_LOG_INFO("Forcing Level 2 telnet reconnection");
        return telnet_iface_stop(iface);
    }

    return telnet_iface_start(iface);
}

/**
 * Check if reconnection should be attempted
 */
bool telnet_iface_should_reconnect(telnet_iface_t *iface)
{
    if (iface == NULL) {
        return false;
    }

    /* Reconnection is needed if we're in error or disconnected state
     * and the thread is not running */
    return (iface->state == TELNET_IFACE_ERROR || iface->state == TELNET_IFACE_DISCONNECTED) &&
           !telnet_thread_is_running(&iface->thread_ctx);
}