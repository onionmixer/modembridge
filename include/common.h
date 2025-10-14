/*
 * common.h - Common definitions and macros for ModemBridge
 *
 * This file contains common definitions, macros, and utilities
 * used across the ModemBridge project.
 */

#ifndef MODEMBRIDGE_COMMON_H
#define MODEMBRIDGE_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>

/* Version information */
#define MODEMBRIDGE_VERSION_MAJOR 1
#define MODEMBRIDGE_VERSION_MINOR 0
#define MODEMBRIDGE_VERSION_PATCH 0
#define MODEMBRIDGE_VERSION "1.0.0"

/* Application name */
#define APP_NAME "modembridge"

/* Buffer sizes */
#define BUFFER_SIZE         4096
#define SMALL_BUFFER_SIZE   256
#define LINE_BUFFER_SIZE    1024

/* Configuration */
#define DEFAULT_CONFIG_FILE "/etc/modembridge.conf"
#define DEFAULT_PID_FILE    "/var/run/modembridge.pid"

/* Return codes */
#define SUCCESS             0
#define ERROR_GENERAL       -1
#define ERROR_INVALID_ARG   -2
#define ERROR_IO            -3
#define ERROR_TIMEOUT       -4
#define ERROR_CONNECTION    -5
#define ERROR_CONFIG        -6

/* Logging macros (prefixed with MB_ to avoid syslog.h conflicts) */
#ifdef DEBUG
#define MB_LOG_DEBUG(fmt, ...) \
    syslog(LOG_DEBUG, "[DEBUG] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define MB_LOG_DEBUG(fmt, ...) do {} while(0)
#endif

#define MB_LOG_INFO(fmt, ...) \
    syslog(LOG_INFO, "[INFO] " fmt, ##__VA_ARGS__)

#define MB_LOG_WARNING(fmt, ...) \
    syslog(LOG_WARNING, "[WARNING] " fmt, ##__VA_ARGS__)

#define MB_LOG_ERROR(fmt, ...) \
    syslog(LOG_ERR, "[ERROR] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

/* Utility macros */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Safe string copy */
#define SAFE_STRNCPY(dst, src, size) do { \
    strncpy(dst, src, size - 1); \
    dst[size - 1] = '\0'; \
} while(0)

/* Connection states */
typedef enum {
    STATE_IDLE,
    STATE_RINGING,
    STATE_NEGOTIATING,
    STATE_CONNECTED,
    STATE_DISCONNECTING,
    STATE_ERROR
} connection_state_t;

/* Global flag for signal handling */
extern volatile sig_atomic_t g_running;
extern volatile sig_atomic_t g_reload_config;

/* Utility functions */
void hexdump(const char *label, const void *data, size_t len);
char *trim_whitespace(char *str);
int daemonize(void);
int write_pid_file(const char *pid_file);
int remove_pid_file(const char *pid_file);

#endif /* MODEMBRIDGE_COMMON_H */
