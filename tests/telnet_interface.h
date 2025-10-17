/*
 * telnet_interface.h - Level 2 Telnet-only data interface
 *
 * Isolated data interface for Level 2 telnet functionality.
 * Provides clean API boundaries and minimizes coupling with other levels.
 */

#ifndef MODEMBRIDGE_TELNET_INTERFACE_H
#define MODEMBRIDGE_TELNET_INTERFACE_H

#ifdef ENABLE_LEVEL2

#include "telnet_thread.h"
#include <stdbool.h>

/* Level 2 Telnet interface states */
typedef enum {
    TELNET_IFACE_DISCONNECTED,
    TELNET_IFACE_CONNECTING,
    TELNET_IFACE_CONNECTED,
    TELNET_IFACE_ERROR
} telnet_iface_state_t;

/* Level 2 Telnet interface context */
typedef struct {
    telnet_thread_ctx_t thread_ctx;
    telnet_iface_state_t state;

    /* Interface configuration */
    char server_host[256];
    int server_port;

    /* Error tracking */
    int last_error_code;
    char last_error_message[256];

    /* Event callbacks */
    void (*on_state_changed)(telnet_iface_state_t old_state, telnet_iface_state_t new_state, void *context);
    void (*on_data_received)(const unsigned char *data, size_t len, void *context);
    void (*on_error)(int error_code, const char *message, void *context);
    void *callback_context;

    /* Statistics */
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    time_t connection_time;
    time_t last_activity;

} telnet_iface_t;

/* Function prototypes */

/**
 * Initialize Level 2 telnet interface
 * @param iface Interface context to initialize
 * @param server_host Telnet server hostname or IP
 * @param server_port Telnet server port
 * @return SUCCESS on success, error code on failure
 */
int telnet_iface_init(telnet_iface_t *iface, const char *server_host, int server_port);

/**
 * Destroy Level 2 telnet interface
 * @param iface Interface context
 */
void telnet_iface_destroy(telnet_iface_t *iface);

/**
 * Start Level 2 telnet interface (connect and begin processing)
 * @param iface Interface context
 * @return SUCCESS on success, error code on failure
 */
int telnet_iface_start(telnet_iface_t *iface);

/**
 * Stop Level 2 telnet interface (disconnect and stop processing)
 * @param iface Interface context
 * @return SUCCESS on success, error code on failure
 */
int telnet_iface_stop(telnet_iface_t *iface);

/**
 * Send data through Level 2 telnet interface
 * @param iface Interface context
 * @param data Data to send
 * @param len Data length
 * @return SUCCESS on success, error code on failure
 */
int telnet_iface_send(telnet_iface_t *iface, const void *data, size_t len);

/**
 * Send text string through Level 2 telnet interface
 * @param iface Interface context
 * @param text Text to send
 * @return SUCCESS on success, error code on failure
 */
int telnet_iface_send_string(telnet_iface_t *iface, const char *text);

/**
 * Get current interface state
 * @param iface Interface context
 * @return Current interface state
 */
telnet_iface_state_t telnet_iface_get_state(telnet_iface_t *iface);

/**
 * Check if interface is connected and ready
 * @param iface Interface context
 * @return true if connected and ready, false otherwise
 */
bool telnet_iface_is_connected(telnet_iface_t *iface);

/**
 * Get last error information
 * @param iface Interface context
 * @param error_code Pointer to store error code (can be NULL)
 * @param error_message Buffer to store error message (can be NULL)
 * @param message_size Size of error_message buffer
 * @return true if there was an error, false otherwise
 */
bool telnet_iface_get_last_error(telnet_iface_t *iface, int *error_code,
                                char *error_message, size_t message_size);

/**
 * Get interface statistics
 * @param iface Interface context
 * @param bytes_sent Pointer to store bytes sent (can be NULL)
 * @param bytes_received Pointer to store bytes received (can be NULL)
 * @param connection_time Pointer to store connection duration (can be NULL)
 * @param last_activity Pointer to store last activity timestamp (can be NULL)
 */
void telnet_iface_get_statistics(telnet_iface_t *iface, uint64_t *bytes_sent,
                                uint64_t *bytes_received, time_t *connection_time,
                                time_t *last_activity);

/**
 * Set interface configuration
 * @param iface Interface context
 * @param connection_timeout Connection timeout in seconds
 * @param reconnect_interval Reconnection interval in seconds
 * @param auto_reconnect Enable automatic reconnection
 */
void telnet_iface_set_config(telnet_iface_t *iface, int connection_timeout,
                            int reconnect_interval, bool auto_reconnect);

/**
 * Set state change callback
 * @param iface Interface context
 * @param callback Callback function
 * @param context User context for callback
 */
void telnet_iface_set_state_callback(telnet_iface_t *iface,
                                     void (*callback)(telnet_iface_state_t, telnet_iface_state_t, void *),
                                     void *context);

/**
 * Set data received callback
 * @param iface Interface context
 * @param callback Callback function
 * @param context User context for callback
 */
void telnet_iface_set_data_callback(telnet_iface_t *iface,
                                   void (*callback)(const unsigned char *, size_t, void *),
                                   void *context);

/**
 * Set error callback
 * @param iface Interface context
 * @param callback Callback function
 * @param context User context for callback
 */
void telnet_iface_set_error_callback(telnet_iface_t *iface,
                                    void (*callback)(int, const char *, void *),
                                    void *context);

/**
 * Get state name as string (for debugging/logging)
 * @param state Interface state
 * @return State name string
 */
const char *telnet_iface_state_to_string(telnet_iface_state_t state);

/**
 * Force reconnection attempt
 * @param iface Interface context
 * @return SUCCESS on success, error code on failure
 */
int telnet_iface_force_reconnect(telnet_iface_t *iface);

/**
 * Check if reconnection should be attempted
 * @param iface Interface context
 * @return true if reconnection is needed/possible, false otherwise
 */
bool telnet_iface_should_reconnect(telnet_iface_t *iface);

#endif /* ENABLE_LEVEL2 */

#endif /* MODEMBRIDGE_TELNET_INTERFACE_H */