/*
 * healthcheck.h - Health check module for ModemBridge
 *
 * Performs diagnostic checks on serial port, modem, and telnet server
 * at startup (one-time only, non-blocking).
 */

#ifndef MODEMBRIDGE_HEALTHCHECK_H
#define MODEMBRIDGE_HEALTHCHECK_H

#include "common.h"
#include "config.h"

/* Health status codes */
typedef enum {
    HEALTH_STATUS_OK,       /* Resource is available and working */
    HEALTH_STATUS_WARNING,  /* Resource has issues but may work */
    HEALTH_STATUS_ERROR,    /* Resource is not available */
    HEALTH_STATUS_UNKNOWN   /* Status could not be determined */
} health_status_t;

/* Health check result for a single resource */
typedef struct {
    health_status_t status;
    char message[SMALL_BUFFER_SIZE];
} health_check_result_t;

/* Complete health check report */
typedef struct {
    health_check_result_t serial_port;
    health_check_result_t serial_init;
    health_check_result_t modem_device;
    health_check_result_t telnet_server;
} health_report_t;

/* Function prototypes */

/**
 * Run complete health check (one-time, at startup)
 * @param cfg Configuration
 * @param report Output report structure
 * @return SUCCESS on successful check execution, error code otherwise
 */
int healthcheck_run(const config_t *cfg, health_report_t *report);

/**
 * Check serial port availability
 * @param device Serial device path (e.g., "/dev/ttyUSB0")
 * @param result Output result structure
 * @return SUCCESS if check completed, error code otherwise
 */
int healthcheck_serial_port(const char *device, health_check_result_t *result);

/**
 * Initialize serial port with configuration settings
 * @param device Serial device path
 * @param cfg Configuration (for baudrate, parity, flow control, etc.)
 * @param result Output result structure
 * @return SUCCESS if check completed, error code otherwise
 */
int healthcheck_serial_init(const char *device, const config_t *cfg,
                            health_check_result_t *result);

/**
 * Check modem device responsiveness (optional, with timeout)
 * @param device Serial device path
 * @param cfg Configuration (for baudrate, etc.)
 * @param result Output result structure
 * @return SUCCESS if check completed, error code otherwise
 */
int healthcheck_modem_device(const char *device, const config_t *cfg,
                             health_check_result_t *result);

/**
 * Check telnet server connectivity (with timeout)
 * @param host Telnet server hostname or IP
 * @param port Telnet server port
 * @param result Output result structure
 * @return SUCCESS if check completed, error code otherwise
 */
int healthcheck_telnet_server(const char *host, int port,
                              health_check_result_t *result);

/**
 * Print health check report to stdout with MODEM_COMMAND execution
 * @param report Health check report
 * @param cfg Configuration (for MODEM_COMMAND execution, can be NULL)
 */
void healthcheck_print_report(const health_report_t *report, const config_t *cfg);

/**
 * Convert health status to string
 * @param status Health status enum
 * @return String representation
 */
const char *healthcheck_status_to_str(health_status_t status);

#endif /* MODEMBRIDGE_HEALTHCHECK_H */
