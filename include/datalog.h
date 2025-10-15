/*
 * datalog.h - Data logging for ModemBridge
 *
 * Provides hex dump logging functionality for debugging data flow
 * between modem and telnet connections. Log format is compatible
 * with otelnet.log format.
 */

#ifndef MODEMBRIDGE_DATALOG_H
#define MODEMBRIDGE_DATALOG_H

#include "common.h"

/* Data direction types */
typedef enum {
    DATALOG_DIR_FROM_MODEM,     /* Data received from modem (serial port) */
    DATALOG_DIR_TO_TELNET,      /* Data sent to telnet server */
    DATALOG_DIR_FROM_TELNET,    /* Data received from telnet server */
    DATALOG_DIR_TO_MODEM,       /* Data sent to modem (serial port) */
    DATALOG_DIR_INTERNAL        /* Internal protocol negotiation/processing */
} datalog_direction_t;

/* Data logger context */
typedef struct {
    FILE *fp;                   /* Log file handle */
    char filename[256];         /* Log file path */
    bool enabled;               /* Logging enabled flag */
    bool session_started;       /* Session marker flag */
} datalog_t;

/* Function prototypes */

/**
 * Initialize data logger
 * @param log Logger context structure
 */
void datalog_init(datalog_t *log);

/**
 * Open log file for writing
 * @param log Logger context
 * @param filename Log file path (default: "modembridge.log")
 * @return SUCCESS on success, error code on failure
 */
int datalog_open(datalog_t *log, const char *filename);

/**
 * Close log file
 * @param log Logger context
 * @return SUCCESS on success, error code on failure
 */
int datalog_close(datalog_t *log);

/**
 * Enable or disable logging
 * @param log Logger context
 * @param enabled True to enable, false to disable
 */
void datalog_set_enabled(datalog_t *log, bool enabled);

/**
 * Check if logging is enabled
 * @param log Logger context
 * @return true if enabled, false otherwise
 */
bool datalog_is_enabled(datalog_t *log);

/**
 * Write session start marker
 * @param log Logger context
 */
void datalog_session_start(datalog_t *log);

/**
 * Write session end marker
 * @param log Logger context
 */
void datalog_session_end(datalog_t *log);

/**
 * Write data to log in hex dump format (otelnet.log compatible)
 * Format: [timestamp][direction] hex_bytes  | ascii_representation
 *
 * @param log Logger context
 * @param direction Data direction (from_modem, to_telnet, etc.)
 * @param data Data buffer to log
 * @param len Data length
 */
void datalog_write(datalog_t *log, datalog_direction_t direction,
                   const void *data, size_t len);

/**
 * Write data with custom label (for internal protocol details)
 * @param log Logger context
 * @param label Custom label string
 * @param data Data buffer to log
 * @param len Data length
 */
void datalog_write_labeled(datalog_t *log, const char *label,
                           const void *data, size_t len);

#endif /* MODEMBRIDGE_DATALOG_H */
