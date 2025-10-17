/*
 * timestamp.c - Modular timestamp transmission functionality for ModemBridge
 *
 * Provides configurable timestamp transmission for Level 1 mode with epoll-based
 * serial write support and flexible timing controls.
 */

#include "timestamp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Global statistics for timestamp transmission */
static int g_total_timestamps_sent = 0;
static int g_total_timestamps_failed = 0;

/**
 * Initialize timestamp control structure with defaults
 */
void timestamp_init(timestamp_ctrl_t *ts)
{
    if (ts == NULL) {
        return;
    }

    memset(ts, 0, sizeof(timestamp_ctrl_t));

    /* Default settings */
    ts->enabled = false;
    ts->connect_time = 0;
    ts->last_sent = 0;
    ts->first_delay = 3;     /* 3 seconds delay before first timestamp */
    ts->interval = 10;        /* 10 seconds interval between timestamps */

    /* Default message format */
    SAFE_STRNCPY(ts->prefix, "[Level 1]", sizeof(ts->prefix));
    SAFE_STRNCPY(ts->suffix, "Active", sizeof(ts->suffix));
    ts->show_date = true;
    ts->show_time = true;

    /* Default transmission settings */
    ts->write_timeout_ms = 1000;  /* 1 second timeout */
    ts->max_retries = 3;           /* Maximum 3 retries */
    ts->retry_delay_ms = 100;      /* 100ms between retries */
}

/**
 * Enable timestamp transmission with custom settings
 */
void timestamp_enable(timestamp_ctrl_t *ts, int first_delay, int interval)
{
    if (ts == NULL) {
        return;
    }

    ts->enabled = true;
    ts->first_delay = first_delay;
    ts->interval = interval;

    /* Reset timing */
    ts->connect_time = 0;
    ts->last_sent = 0;

    MB_LOG_INFO("Timestamp transmission enabled: first_delay=%d sec, interval=%d sec",
                first_delay, interval);
}

/**
 * Disable timestamp transmission
 */
void timestamp_disable(timestamp_ctrl_t *ts)
{
    if (ts == NULL) {
        return;
    }

    ts->enabled = false;
    MB_LOG_INFO("Timestamp transmission disabled");
}

/**
 * Configure timestamp message format
 */
void timestamp_set_format(timestamp_ctrl_t *ts, const char *prefix, const char *suffix,
                         bool show_date, bool show_time)
{
    if (ts == NULL) {
        return;
    }

    if (prefix != NULL) {
        SAFE_STRNCPY(ts->prefix, prefix, sizeof(ts->prefix));
    }
    if (suffix != NULL) {
        SAFE_STRNCPY(ts->suffix, suffix, sizeof(ts->suffix));
    }

    ts->show_date = show_date;
    ts->show_time = show_time;

    MB_LOG_DEBUG("Timestamp format configured: prefix='%s', suffix='%s', date=%d, time=%d",
                 ts->prefix, ts->suffix, show_date, show_time);
}

/**
 * Configure transmission settings
 */
void timestamp_set_transmission(timestamp_ctrl_t *ts, int write_timeout_ms,
                                int max_retries, int retry_delay_ms)
{
    if (ts == NULL) {
        return;
    }

    ts->write_timeout_ms = write_timeout_ms;
    ts->max_retries = max_retries;
    ts->retry_delay_ms = retry_delay_ms;

    MB_LOG_DEBUG("Timestamp transmission settings: timeout=%dms, retries=%d, retry_delay=%dms",
                 write_timeout_ms, max_retries, retry_delay_ms);
}

/**
 * Mark modem as ONLINE (start tracking connect time)
 */
void timestamp_set_online(timestamp_ctrl_t *ts)
{
    if (ts == NULL) {
        MB_LOG_ERROR("timestamp_set_online: ts is NULL");
        return;
    }

    if (!ts->enabled) {
        MB_LOG_WARNING("timestamp_set_online: timestamp transmission is disabled");
        return;
    }

    ts->connect_time = time(NULL);
    ts->last_sent = 0;  /* Reset to trigger first timestamp */

    MB_LOG_INFO("Timestamp: modem ONLINE detected, first timestamp in %d seconds",
                ts->first_delay);
    MB_LOG_DEBUG("Timestamp tracking started: connect_time=%ld, first_delay=%d, interval=%d",
                 (long)ts->connect_time, ts->first_delay, ts->interval);
}

/**
 * Mark modem as OFFLINE (reset tracking)
 */
void timestamp_set_offline(timestamp_ctrl_t *ts)
{
    if (ts == NULL) {
        return;
    }

    ts->connect_time = 0;
    ts->last_sent = 0;

    MB_LOG_DEBUG("Timestamp state reset (modem offline)");
}

/**
 * Check if timestamp should be sent now
 */
bool timestamp_should_send(timestamp_ctrl_t *ts)
{
    if (ts == NULL) {
        MB_LOG_DEBUG("timestamp_should_send: ts is NULL");
        return false;
    }

    if (!ts->enabled) {
        MB_LOG_DEBUG("timestamp_should_send: timestamp transmission disabled");
        return false;
    }

    if (ts->connect_time == 0) {
        MB_LOG_DEBUG("timestamp_should_send: connect_time is 0 (not online)");
        return false;
    }

    time_t now = time(NULL);
    time_t elapsed = now - ts->connect_time;

    /* First timestamp: wait first_delay seconds from connect */
    if (ts->last_sent == 0) {
        if (elapsed >= ts->first_delay) {
            MB_LOG_DEBUG("timestamp_should_send: FIRST timestamp due (elapsed=%ld >= first_delay=%d)",
                         (long)elapsed, ts->first_delay);
            return true;
        } else {
            MB_LOG_DEBUG("timestamp_should_send: first timestamp not due yet (elapsed=%ld < first_delay=%d)",
                         (long)elapsed, ts->first_delay);
            return false;
        }
    }

    /* Subsequent timestamps: wait interval seconds from last sent */
    time_t since_last = now - ts->last_sent;
    if (since_last >= ts->interval) {
        MB_LOG_DEBUG("timestamp_should_send: subsequent timestamp due (since_last=%ld >= interval=%d)",
                     (long)since_last, ts->interval);
        return true;
    } else {
        MB_LOG_DEBUG("timestamp_should_send: subsequent timestamp not due yet (since_last=%ld < interval=%d)",
                     (long)since_last, ts->interval);
        return false;
    }
}

/**
 * Format timestamp message into buffer
 */
int timestamp_format_message(timestamp_ctrl_t *ts, char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm *tm_info;
    int pos = 0;

    if (ts == NULL || buffer == NULL || buffer_size == 0) {
        return 0;
    }

    now = time(NULL);
    tm_info = localtime(&now);

    /* Start with CRLF for clean line start */
    pos += snprintf(buffer + pos, buffer_size - pos, "\r\n");

    /* Add prefix if specified */
    if (strlen(ts->prefix) > 0) {
        pos += snprintf(buffer + pos, buffer_size - pos, "%s", ts->prefix);
        pos += snprintf(buffer + pos, buffer_size - pos, " ");
    }

    /* Add opening bracket */
    pos += snprintf(buffer + pos, buffer_size - pos, "[");

    /* Add date if requested */
    if (ts->show_date) {
        pos += snprintf(buffer + pos, buffer_size - pos, "%04d-%02d-%02d",
                        tm_info->tm_year + 1900,
                        tm_info->tm_mon + 1,
                        tm_info->tm_mday);

        if (ts->show_time) {
            pos += snprintf(buffer + pos, buffer_size - pos, " ");
        }
    }

    /* Add time if requested */
    if (ts->show_time) {
        pos += snprintf(buffer + pos, buffer_size - pos, "%02d:%02d:%02d",
                        tm_info->tm_hour,
                        tm_info->tm_min,
                        tm_info->tm_sec);
    }

    /* Add closing bracket */
    pos += snprintf(buffer + pos, buffer_size - pos, "]");

    /* Add suffix if specified */
    if (strlen(ts->suffix) > 0) {
        pos += snprintf(buffer + pos, buffer_size - pos, " %s", ts->suffix);
    }

    /* End with CRLF */
    pos += snprintf(buffer + pos, buffer_size - pos, "\r\n");

    return pos;  /* Return length (excluding null terminator) */
}

/**
 * Send timestamp to serial port using epoll-based write
 */
timestamp_result_t timestamp_send(serial_port_t *port, timestamp_ctrl_t *ts)
{
    char timestamp_msg[256];
    int msg_len;
    ssize_t sent;
    int retry_count = 0;

    if (port == NULL || ts == NULL) {
        return TIMESTAMP_ERROR;
    }

    if (!ts->enabled) {
        return TIMESTAMP_DISABLED;
    }

    /* Format timestamp message */
    msg_len = timestamp_format_message(ts, timestamp_msg, sizeof(timestamp_msg));
    if (msg_len <= 0) {
        MB_LOG_ERROR("Failed to format timestamp message");
        return TIMESTAMP_ERROR;
    }

    MB_LOG_DEBUG("Sending timestamp: %.*s", msg_len, timestamp_msg);

    /* Retry loop for robust transmission */
    while (retry_count < ts->max_retries) {
        /* Send using epoll-based write */
        sent = serial_write_with_epoll(port, (unsigned char *)timestamp_msg, msg_len,
                                      ts->write_timeout_ms);

        if (sent > 0) {
            /* Success */
            ts->last_sent = time(NULL);
            g_total_timestamps_sent++;

            MB_LOG_INFO("Timestamp sent successfully: %zd bytes", sent);
            MB_LOG_DEBUG("Next timestamp in %d seconds", ts->interval);
            return TIMESTAMP_SUCCESS;
        } else if (sent == ERROR_TIMEOUT) {
            /* Timeout */
            MB_LOG_WARNING("Timestamp send timeout (attempt %d/%d)",
                          retry_count + 1, ts->max_retries);

            if (retry_count < ts->max_retries - 1) {
                usleep(ts->retry_delay_ms * 1000);  /* Convert to microseconds */
            }
        } else {
            /* Other error */
            MB_LOG_ERROR("Timestamp send failed: %zd (attempt %d/%d)",
                        sent, retry_count + 1, ts->max_retries);

            if (retry_count < ts->max_retries - 1) {
                usleep(ts->retry_delay_ms * 1000);  /* Convert to microseconds */
            }
        }

        retry_count++;
    }

    /* All retries failed */
    g_total_timestamps_failed++;
    MB_LOG_ERROR("Timestamp send failed after %d retries", ts->max_retries);
    return TIMESTAMP_ERROR;
}

/**
 * Send timestamp with custom message
 */
timestamp_result_t timestamp_send_custom(serial_port_t *port, timestamp_ctrl_t *ts,
                                        const char *custom_message)
{
    char full_message[256];
    ssize_t sent;
    int retry_count = 0;

    if (port == NULL || ts == NULL || custom_message == NULL) {
        return TIMESTAMP_ERROR;
    }

    if (!ts->enabled) {
        return TIMESTAMP_DISABLED;
    }

    /* Format custom message with CRLF wrapping */
    int msg_len = snprintf(full_message, sizeof(full_message), "\r\n%s\r\n", custom_message);
    if (msg_len <= 0 || (size_t)msg_len >= sizeof(full_message)) {
        MB_LOG_ERROR("Failed to format custom timestamp message");
        return TIMESTAMP_ERROR;
    }

    MB_LOG_INFO("Sending custom timestamp: %s", custom_message);

    /* Retry loop for robust transmission */
    while (retry_count < ts->max_retries) {
        /* Send using epoll-based write */
        sent = serial_write_with_epoll(port, (unsigned char *)full_message, msg_len,
                                      ts->write_timeout_ms);

        if (sent > 0) {
            /* Success */
            ts->last_sent = time(NULL);
            g_total_timestamps_sent++;

            MB_LOG_INFO("Custom timestamp sent successfully: %zd bytes", sent);
            return TIMESTAMP_SUCCESS;
        } else if (sent == ERROR_TIMEOUT) {
            MB_LOG_WARNING("Custom timestamp send timeout (attempt %d/%d)",
                          retry_count + 1, ts->max_retries);

            if (retry_count < ts->max_retries - 1) {
                usleep(ts->retry_delay_ms * 1000);
            }
        } else {
            MB_LOG_ERROR("Custom timestamp send failed: %zd (attempt %d/%d)",
                        sent, retry_count + 1, ts->max_retries);

            if (retry_count < ts->max_retries - 1) {
                usleep(ts->retry_delay_ms * 1000);
            }
        }

        retry_count++;
    }

    /* All retries failed */
    g_total_timestamps_failed++;
    MB_LOG_ERROR("Custom timestamp send failed after %d retries", ts->max_retries);
    return TIMESTAMP_ERROR;
}

/**
 * Get time until next timestamp is due
 */
int timestamp_get_next_due(timestamp_ctrl_t *ts)
{
    if (ts == NULL || !ts->enabled || ts->connect_time == 0) {
        return -1;
    }

    time_t now = time(NULL);

    /* First timestamp */
    if (ts->last_sent == 0) {
        int due = ts->first_delay - (now - ts->connect_time);
        return (due > 0) ? due : 0;
    }

    /* Subsequent timestamps */
    int due = ts->interval - (now - ts->last_sent);
    return (due > 0) ? due : 0;
}

/**
 * Get timestamp statistics
 */
void timestamp_get_stats(timestamp_ctrl_t *ts, int *total_sent, int *total_failed)
{
    if (ts == NULL) {
        return;
    }

    if (total_sent != NULL) {
        *total_sent = g_total_timestamps_sent;
    }
    if (total_failed != NULL) {
        *total_failed = g_total_timestamps_failed;
    }
}