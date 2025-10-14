/*
 * config.h - Configuration management for ModemBridge
 *
 * Handles parsing and management of configuration file
 * (modembridge.conf)
 */

#ifndef MODEMBRIDGE_CONFIG_H
#define MODEMBRIDGE_CONFIG_H

#include "common.h"
#include <termios.h>

/* Parity types */
typedef enum {
    PARITY_NONE,
    PARITY_EVEN,
    PARITY_ODD
} parity_t;

/* Flow control types */
typedef enum {
    FLOW_NONE,
    FLOW_XONXOFF,
    FLOW_RTSCTS,
    FLOW_BOTH
} flow_control_t;

/* Configuration structure */
typedef struct {
    /* Serial port settings */
    char comport[SMALL_BUFFER_SIZE];
    speed_t baudrate;           /* termios speed_t type */
    int baudrate_value;         /* Actual numeric value for display */
    parity_t parity;
    int data_bits;
    int stop_bits;
    flow_control_t flow_control;

    /* Telnet settings */
    char telnet_host[SMALL_BUFFER_SIZE];
    int telnet_port;

    /* Runtime options */
    bool daemon_mode;
    char pid_file[SMALL_BUFFER_SIZE];
    int log_level;
} config_t;

/* Function prototypes */

/**
 * Initialize configuration with default values
 * @param cfg Configuration structure to initialize
 */
void config_init(config_t *cfg);

/**
 * Load configuration from file
 * @param cfg Configuration structure to populate
 * @param config_file Path to configuration file
 * @return SUCCESS on success, error code on failure
 */
int config_load(config_t *cfg, const char *config_file);

/**
 * Validate configuration values
 * @param cfg Configuration structure to validate
 * @return SUCCESS on success, error code on failure
 */
int config_validate(const config_t *cfg);

/**
 * Print configuration to log
 * @param cfg Configuration structure to print
 */
void config_print(const config_t *cfg);

/**
 * Free configuration resources
 * @param cfg Configuration structure to free
 */
void config_free(config_t *cfg);

/**
 * Convert baudrate value to termios speed_t
 * @param baudrate Numeric baudrate (e.g., 9600)
 * @return termios speed_t constant (e.g., B9600)
 */
speed_t config_baudrate_to_speed(int baudrate);

/**
 * Convert string to parity enum
 * @param str Parity string ("NONE", "EVEN", "ODD")
 * @return parity_t enum value
 */
parity_t config_str_to_parity(const char *str);

/**
 * Convert string to flow control enum
 * @param str Flow control string ("NONE", "XON/XOFF", "RTS/CTS", "BOTH")
 * @return flow_control_t enum value
 */
flow_control_t config_str_to_flow(const char *str);

/**
 * Convert parity enum to string
 * @param parity Parity enum value
 * @return String representation
 */
const char *config_parity_to_str(parity_t parity);

/**
 * Convert flow control enum to string
 * @param flow Flow control enum value
 * @return String representation
 */
const char *config_flow_to_str(flow_control_t flow);

#endif /* MODEMBRIDGE_CONFIG_H */
