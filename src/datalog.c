/*
 * datalog.c - Data logging implementation for ModemBridge
 */

#include "datalog.h"
#include <time.h>
#include <ctype.h>

/* Direction label strings */
static const char *direction_labels[] = {
    [DATALOG_DIR_FROM_MODEM]   = "from_modem",
    [DATALOG_DIR_TO_TELNET]    = "to_telnet",
    [DATALOG_DIR_FROM_TELNET]  = "from_telnet",
    [DATALOG_DIR_TO_MODEM]     = "to_modem",
    [DATALOG_DIR_INTERNAL]     = "internal"
};

/**
 * Get current timestamp string
 */
static void get_timestamp(char *buf, size_t size)
{
    time_t now;
    struct tm *tm_info;

    time(&now);
    tm_info = localtime(&now);

    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * Initialize data logger
 */
void datalog_init(datalog_t *log)
{
    if (log == NULL) {
        return;
    }

    memset(log, 0, sizeof(datalog_t));
    log->fp = NULL;
    log->enabled = false;
    log->session_started = false;
}

/**
 * Open log file for writing
 */
int datalog_open(datalog_t *log, const char *filename)
{
    if (log == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Close existing file if open */
    if (log->fp != NULL) {
        datalog_close(log);
    }

    /* Use default filename if not specified */
    if (filename == NULL) {
        filename = "modembridge.log";
    }

    /* Open log file in append mode */
    log->fp = fopen(filename, "a");
    if (log->fp == NULL) {
        MB_LOG_ERROR("Failed to open data log file: %s: %s", filename, strerror(errno));
        return ERROR_IO;
    }

    /* Save filename */
    SAFE_STRNCPY(log->filename, filename, sizeof(log->filename));

    /* Enable buffering for better performance */
    setvbuf(log->fp, NULL, _IOFBF, 8192);

    log->enabled = true;
    log->session_started = false;

    MB_LOG_INFO("Data logging opened: %s", filename);

    return SUCCESS;
}

/**
 * Close log file
 */
int datalog_close(datalog_t *log)
{
    if (log == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (log->fp == NULL) {
        return SUCCESS;
    }

    /* Write session end marker if session was started */
    if (log->session_started) {
        datalog_session_end(log);
    }

    fflush(log->fp);
    fclose(log->fp);
    log->fp = NULL;
    log->enabled = false;

    MB_LOG_INFO("Data logging closed: %s", log->filename);

    return SUCCESS;
}

/**
 * Enable or disable logging
 */
void datalog_set_enabled(datalog_t *log, bool enabled)
{
    if (log == NULL) {
        return;
    }

    log->enabled = enabled;
}

/**
 * Check if logging is enabled
 */
bool datalog_is_enabled(datalog_t *log)
{
    if (log == NULL) {
        return false;
    }

    return log->enabled && log->fp != NULL;
}

/**
 * Write session start marker
 */
void datalog_session_start(datalog_t *log)
{
    char timestamp[64];

    if (!datalog_is_enabled(log)) {
        return;
    }

    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(log->fp, "\n[%s] === Session started ===\n", timestamp);
    fflush(log->fp);

    log->session_started = true;
}

/**
 * Write session end marker
 */
void datalog_session_end(datalog_t *log)
{
    char timestamp[64];

    if (!datalog_is_enabled(log)) {
        return;
    }

    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(log->fp, "[%s] === Session ended ===\n", timestamp);
    fflush(log->fp);

    log->session_started = false;
}

/**
 * Write data to log in hex dump format (otelnet.log compatible)
 * Format: [timestamp][direction] hex_bytes  | ascii_representation
 */
void datalog_write(datalog_t *log, datalog_direction_t direction,
                   const void *data, size_t len)
{
    const unsigned char *bytes = (const unsigned char *)data;
    char timestamp[64];
    const char *dir_label;

    if (!datalog_is_enabled(log) || data == NULL || len == 0) {
        return;
    }

    /* Get direction label */
    if (direction < 0 || direction >= ARRAY_SIZE(direction_labels)) {
        dir_label = "unknown";
    } else {
        dir_label = direction_labels[direction];
    }

    get_timestamp(timestamp, sizeof(timestamp));

    /* Write data in 16-byte chunks (otelnet.log format) */
    for (size_t i = 0; i < len; i += 16) {
        char hex_buf[64] = {0};
        char ascii_buf[20] = {0};
        size_t chunk_len = MIN(16, len - i);
        size_t hex_pos = 0;

        /* Build hex representation */
        for (size_t j = 0; j < chunk_len; j++) {
            hex_pos += sprintf(hex_buf + hex_pos, "%02x ", bytes[i + j]);
        }

        /* Build ASCII representation */
        for (size_t j = 0; j < chunk_len; j++) {
            unsigned char c = bytes[i + j];
            ascii_buf[j] = isprint(c) ? c : '.';
        }
        ascii_buf[chunk_len] = '\0';

        /* Write line in otelnet.log format */
        /* Format: [timestamp][direction] hex_bytes  | ascii */
        fprintf(log->fp, "[%s][%s] %-48s | %s\n",
                timestamp, dir_label, hex_buf, ascii_buf);
    }

    /* Flush after each write to ensure data is written immediately */
    fflush(log->fp);
}

/**
 * Write data with custom label (for internal protocol details)
 */
void datalog_write_labeled(datalog_t *log, const char *label,
                           const void *data, size_t len)
{
    const unsigned char *bytes = (const unsigned char *)data;
    char timestamp[64];

    if (!datalog_is_enabled(log) || label == NULL || data == NULL || len == 0) {
        return;
    }

    get_timestamp(timestamp, sizeof(timestamp));

    /* Write data in 16-byte chunks with custom label */
    for (size_t i = 0; i < len; i += 16) {
        char hex_buf[64] = {0};
        char ascii_buf[20] = {0};
        size_t chunk_len = MIN(16, len - i);
        size_t hex_pos = 0;

        /* Build hex representation */
        for (size_t j = 0; j < chunk_len; j++) {
            hex_pos += sprintf(hex_buf + hex_pos, "%02x ", bytes[i + j]);
        }

        /* Build ASCII representation */
        for (size_t j = 0; j < chunk_len; j++) {
            unsigned char c = bytes[i + j];
            ascii_buf[j] = isprint(c) ? c : '.';
        }
        ascii_buf[chunk_len] = '\0';

        /* Write line with custom label */
        fprintf(log->fp, "[%s][%s] %-48s | %s\n",
                timestamp, label, hex_buf, ascii_buf);
    }

    fflush(log->fp);
}
