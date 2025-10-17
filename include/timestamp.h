/*
 * timestamp.h - Modular timestamp transmission functionality for ModemBridge
 *
 * Provides configurable timestamp transmission for Level 1 mode with epoll-based
 * serial write support and flexible timing controls.
 */

#ifndef MODEMBRIDGE_TIMESTAMP_H
#define MODEMBRIDGE_TIMESTAMP_H

#include "common.h"
#include "serial.h"
#include <time.h>
#include <stdbool.h>

/* Timestamp transmission control structure */
typedef struct {
    bool enabled;               /* Timestamp transmission enabled */
    time_t connect_time;        /* When modem went ONLINE */
    time_t last_sent;           /* Last timestamp sent time */
    int first_delay;            /* Delay before first timestamp (seconds) */
    int interval;               /* Interval between timestamps (seconds) */

    /* Message formatting */
    char prefix[64];            /* Message prefix (e.g., "[Level 1]") */
    char suffix[64];            /* Message suffix (e.g., "Active") */
    bool show_date;             /* Include date in timestamp */
    bool show_time;             /* Include time in timestamp */

    /* Transmission settings */
    int write_timeout_ms;       /* Timeout for epoll-based write (milliseconds) */
    int max_retries;            /* Maximum retry attempts on write failure */
    int retry_delay_ms;         /* Delay between retries (milliseconds) */
} timestamp_ctrl_t;

/* Timestamp transmission status */
typedef enum {
    TIMESTAMP_SUCCESS,          /* Timestamp sent successfully */
    TIMESTAMP_TIMEOUT,          /* Write timeout occurred */
    TIMESTAMP_ERROR,           /* Write error occurred */
    TIMESTAMP_DISABLED,        /* Timestamp transmission is disabled */
    TIMESTAMP_NOT_DUE         /* Not time to send yet */
} timestamp_result_t;

/* Function prototypes */

/**
 * Initialize timestamp control structure with defaults
 * @param ts Timestamp control structure to initialize
 */
void timestamp_init(timestamp_ctrl_t *ts);

/**
 * Enable timestamp transmission with custom settings
 * @param ts Timestamp control structure
 * @param first_delay Delay before first timestamp (seconds, default=3)
 * @param interval Interval between timestamps (seconds, default=10)
 */
void timestamp_enable(timestamp_ctrl_t *ts, int first_delay, int interval);

/**
 * Disable timestamp transmission
 * @param ts Timestamp control structure
 */
void timestamp_disable(timestamp_ctrl_t *ts);

/**
 * Configure timestamp message format
 * @param ts Timestamp control structure
 * @param prefix Message prefix (e.g., "[Level 1]")
 * @param suffix Message suffix (e.g., "Active")
 * @param show_date Include date in timestamp (true/false)
 * @param show_time Include time in timestamp (true/false)
 */
void timestamp_set_format(timestamp_ctrl_t *ts, const char *prefix, const char *suffix,
                         bool show_date, bool show_time);

/**
 * Configure transmission settings
 * @param ts Timestamp control structure
 * @param write_timeout_ms Write timeout in milliseconds (default=1000)
 * @param max_retries Maximum retry attempts (default=3)
 * @param retry_delay_ms Delay between retries in milliseconds (default=100)
 */
void timestamp_set_transmission(timestamp_ctrl_t *ts, int write_timeout_ms,
                                int max_retries, int retry_delay_ms);

/**
 * Mark modem as ONLINE (start tracking connect time)
 * @param ts Timestamp control structure
 */
void timestamp_set_online(timestamp_ctrl_t *ts);

/**
 * Mark modem as OFFLINE (reset tracking)
 * @param ts Timestamp control structure
 */
void timestamp_set_offline(timestamp_ctrl_t *ts);

/**
 * Check if timestamp should be sent now
 * @param ts Timestamp control structure
 * @return true if timestamp should be sent, false otherwise
 */
bool timestamp_should_send(timestamp_ctrl_t *ts);

/**
 * Format timestamp message into buffer
 * @param ts Timestamp control structure
 * @param buffer Buffer to store formatted message
 * @param buffer_size Size of buffer
 * @return Number of characters written (excluding null terminator)
 */
int timestamp_format_message(timestamp_ctrl_t *ts, char *buffer, size_t buffer_size);

/**
 * Send timestamp to serial port using epoll-based write
 * @param port Serial port structure
 * @param ts Timestamp control structure
 * @return timestamp_result_t indicating success or failure
 */
timestamp_result_t timestamp_send(serial_port_t *port, timestamp_ctrl_t *ts);

/**
 * Send timestamp with custom message
 * @param port Serial port structure
 * @param ts Timestamp control structure
 * @param custom_message Custom message to send (NULL for default format)
 * @return timestamp_result_t indicating success or failure
 */
timestamp_result_t timestamp_send_custom(serial_port_t *port, timestamp_ctrl_t *ts,
                                        const char *custom_message);

/**
 * Get time until next timestamp is due
 * @param ts Timestamp control structure
 * @return Seconds until next timestamp, -1 if disabled or not connected
 */
int timestamp_get_next_due(timestamp_ctrl_t *ts);

/**
 * Get timestamp statistics
 * @param ts Timestamp control structure
 * @param total_sent Pointer to store total timestamps sent (optional)
 * @param total_failed Pointer to store total failed attempts (optional)
 */
void timestamp_get_stats(timestamp_ctrl_t *ts, int *total_sent, int *total_failed);

#endif /* MODEMBRIDGE_TIMESTAMP_H */