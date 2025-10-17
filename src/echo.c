/*
 * echo.c - Client echo functionality with timestamp formatting for ModemBridge
 *
 * Provides configurable echo functionality that intercepts client input in Level 1 mode
 * and echoes it back with timestamp formatting: "[timestamp][from server] DATA"
 */

#include "echo.h"
#include "util.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * Initialize echo control structure with defaults
 */
void echo_init(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        return;
    }

    memset(echo, 0, sizeof(echo_ctrl_t));

    /* Initialize common transmission control */
    util_transmission_init(&echo->transmission);

    /* Echo-specific settings */
    echo->connect_time = 0;
    echo->last_echo = 0;
    echo->buffer_len = 0;

    /* Set echo-specific defaults */
    echo->first_delay = 5;      /* 5 seconds delay before first echo */
    echo->min_interval = 2;     /* Minimum 2 seconds between echoes */
}

/**
 * Enable echo functionality with custom settings
 */
void echo_enable(echo_ctrl_t *echo, bool immediate, int first_delay, int min_interval)
{
    if (echo == NULL) {
        return;
    }

    echo->first_delay = first_delay;
    echo->min_interval = min_interval;

    /* Use common transmission control */
    util_transmission_enable(&echo->transmission, immediate,
                             first_delay * 1000,  /* Convert to ms */
                             min_interval * 1000);

    /* Reset echo-specific timing */
    echo->connect_time = 0;
    echo->last_echo = 0;
    echo->buffer_len = 0;

    MB_LOG_INFO("Echo functionality enabled: immediate=%d, first_delay=%d sec, min_interval=%d sec",
                immediate, first_delay, min_interval);
}

/**
 * Disable echo functionality
 */
void echo_disable(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        return;
    }

    util_transmission_disable(&echo->transmission);
    echo->buffer_len = 0;

    MB_LOG_INFO("Echo functionality disabled");
}

/**
 * Mark client as ONLINE (start tracking connect time)
 */
void echo_set_online(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        MB_LOG_ERROR("echo_set_online: echo is NULL");
        return;
    }

    if (!echo->transmission.enabled) {
        MB_LOG_DEBUG("echo_set_online: echo functionality is disabled");
        return;
    }

    echo->connect_time = time(NULL);
    echo->last_echo = 0;  /* Reset to trigger first echo */
    echo->buffer_len = 0;

    /* Use common transmission control */
    util_transmission_set_online(&echo->transmission);

    MB_LOG_INFO("Echo: client ONLINE detected, first echo in %d seconds", echo->first_delay);
}

/**
 * Mark client as OFFLINE (reset tracking)
 */
void echo_set_offline(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        return;
    }

    util_transmission_set_offline(&echo->transmission);
    echo->connect_time = 0;
    echo->last_echo = 0;
    echo->buffer_len = 0;

    MB_LOG_DEBUG("Echo state reset (client offline)");
}

/**
 * Configure echo message prefix
 */
void echo_set_prefix(echo_ctrl_t *echo, const char *prefix)
{
    if (echo == NULL) {
        return;
    }

    if (prefix != NULL) {
        util_transmission_set_prefix(&echo->transmission, prefix);
    }

    MB_LOG_DEBUG("Echo prefix configured: '%s'", prefix);
}

/**
 * Configure transmission settings
 */
void echo_set_transmission(echo_ctrl_t *echo, int write_timeout_ms,
                          int max_retries, int retry_delay_ms)
{
    if (echo == NULL) {
        return;
    }

    util_transmission_set_timing(&echo->transmission, write_timeout_ms,
                                 max_retries, retry_delay_ms);

    MB_LOG_DEBUG("Echo transmission settings: timeout=%dms, retries=%d, retry_delay=%dms",
                 write_timeout_ms, max_retries, retry_delay_ms);
}

/**
 * Check if echo should be sent now
 */
bool echo_should_send(echo_ctrl_t *echo)
{
    bool should_send;

    if (echo == NULL) {
        MB_LOG_DEBUG("echo_should_send: echo is NULL");
        return false;
    }

    if (!echo->transmission.enabled) {
        MB_LOG_DEBUG("echo_should_send: echo functionality disabled");
        return false;
    }

    if (!echo->transmission.online_mode || echo->connect_time == 0) {
        MB_LOG_DEBUG("echo_should_send: client is not online");
        return false;
    }

    /* Use common transmission timing logic */
    should_send = util_transmission_should_send(&echo->transmission);
    if (should_send) {
        echo->last_echo = time(NULL);
        return true;
    }

    return false;
}

/**
 * Format echo message into buffer
 */
int echo_format_message(echo_ctrl_t *echo, const char *client_data,
                       size_t client_len, char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm *tm_info;
    char timestamp[64];  /* Increased size to prevent format truncation */
    char content[UTIL_MAX_MESSAGE_LEN];
    int result;

    if (echo == NULL || buffer == NULL || buffer_size == 0) {
        return 0;
    }

    /* Create timestamp */
    now = time(NULL);
    tm_info = localtime(&now);
    snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d]",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);

    /* Create content with timestamp and client data */
    if (client_data != NULL && client_len > 0) {
        snprintf(content, sizeof(content), "%s %.*s", timestamp, (int)client_len, client_data);
    } else {
        snprintf(content, sizeof(content), "%s", timestamp);
    }

    /* Use common transmission formatting */
    result = util_transmission_format_message(&echo->transmission, content, buffer, buffer_size);

    return result;
}

/**
 * Send formatted echo message using epoll-based serial write
 */
echo_result_t echo_send_formatted(echo_ctrl_t *echo, serial_port_t *port,
                                 const char *client_data, size_t len)
{
    char content[UTIL_MAX_MESSAGE_LEN];
    util_result_t result;

    if (port == NULL || echo == NULL) {
        return ECHO_ERROR;
    }

    if (!echo->transmission.enabled) {
        return ECHO_DISABLED;
    }

    if (!echo->transmission.online_mode) {
        MB_LOG_DEBUG("echo_send_formatted: client not online");
        return ECHO_ERROR;
    }

    /* Create content with timestamp */
    if (client_data != NULL && len > 0) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(content, sizeof(content), "[%04d-%02d-%02d %02d:%02d:%02d] %.*s",
                 tm_info->tm_year + 1900,
                 tm_info->tm_mon + 1,
                 tm_info->tm_mday,
                 tm_info->tm_hour,
                 tm_info->tm_min,
                 tm_info->tm_sec,
                 (int)len, client_data);
    } else {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(content, sizeof(content), "[%04d-%02d-%02d %02d:%02d:%02d]",
                 tm_info->tm_year + 1900,
                 tm_info->tm_mon + 1,
                 tm_info->tm_mday,
                 tm_info->tm_hour,
                 tm_info->tm_min,
                 tm_info->tm_sec);
    }

    /* Use common transmission sending */
    result = util_transmission_send(port, &echo->transmission, content);

    echo->last_echo = time(NULL);

    return (echo_result_t)result;
}

/**
 * Process client data and optionally echo it back
 */
echo_result_t echo_process_client_data(echo_ctrl_t *echo, serial_port_t *port,
                                      const unsigned char *data, size_t len)
{
    if (echo == NULL || port == NULL || data == NULL || len == 0) {
        return ECHO_INVALID_PARAM;
    }

    if (!echo->transmission.enabled || !echo->transmission.online_mode) {
        return ECHO_DISABLED;
    }

    MB_LOG_DEBUG("Processing client data for echo: %zu bytes", len);

    /* Handle immediate echo mode */
    if (echo->transmission.immediate && echo_should_send(echo)) {
        return echo_send_formatted(echo, port, (const char *)data, len);
    }

    /* Handle buffered echo mode */
    if (!echo->transmission.immediate) {
        /* Add data to buffer */
        size_t space_left = sizeof(echo->buffer) - echo->buffer_len;
        size_t to_copy = (len < space_left) ? len : space_left;

        if (to_copy > 0) {
            memcpy(echo->buffer + echo->buffer_len, data, to_copy);
            echo->buffer_len += to_copy;

            MB_LOG_DEBUG("Added %zu bytes to echo buffer (total: %zu bytes)",
                        to_copy, echo->buffer_len);
        }

        /* Check if buffer is full or should send now */
        if (echo_is_buffer_full(echo) || echo_should_send(echo)) {
            return echo_flush_buffer(echo, port);
        }
    }

    return ECHO_SUCCESS;
}

/**
 * Flush buffered echo data
 */
echo_result_t echo_flush_buffer(echo_ctrl_t *echo, serial_port_t *port)
{
    echo_result_t result;

    if (echo == NULL || port == NULL) {
        return ECHO_INVALID_PARAM;
    }

    if (echo->buffer_len == 0) {
        return ECHO_SUCCESS;  /* Nothing to flush */
    }

    MB_LOG_DEBUG("Flushing echo buffer: %zu bytes", echo->buffer_len);

    /* Send buffered data */
    result = echo_send_formatted(echo, port, echo->buffer, echo->buffer_len);

    /* Reset buffer regardless of success/failure */
    echo_reset_buffer(echo);

    return result;
}

/**
 * Reset echo buffer
 */
void echo_reset_buffer(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        return;
    }

    echo->buffer_len = 0;
    memset(echo->buffer, 0, sizeof(echo->buffer));

    MB_LOG_DEBUG("Echo buffer reset");
}

/**
 * Check if echo buffer is full
 */
bool echo_is_buffer_full(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        return true;
    }

    return echo->buffer_len >= (sizeof(echo->buffer) - 64);  /* Leave some margin */
}

/**
 * Get echo statistics
 */
void echo_get_stats(echo_ctrl_t *echo, int *total_sent, int *total_failed)
{
    if (echo == NULL) {
        return;
    }

    /* Use common transmission statistics */
    util_transmission_get_stats(&echo->transmission, total_sent, total_failed);
}

/**
 * Print echo status for debugging
 */
void echo_print_status(echo_ctrl_t *echo)
{
    if (echo == NULL) {
        printf("Echo: NULL pointer\n");
        return;
    }

    /* Use common transmission status printing */
    util_transmission_print_status(&echo->transmission, "Echo");

    /* Add echo-specific information */
    printf("  First delay: %d seconds\n", echo->first_delay);
    printf("  Min interval: %d seconds\n", echo->min_interval);
    printf("  Buffer length: %zu bytes\n", echo->buffer_len);

    if (echo->connect_time > 0) {
        time_t now = time(NULL);
        printf("  Connected for: %ld seconds\n", (long)(now - echo->connect_time));
    }

    if (echo->last_echo > 0) {
        time_t now = time(NULL);
        printf("  Last echo: %ld seconds ago\n", (long)(now - echo->last_echo));
    }
}