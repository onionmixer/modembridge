/*
 * config.c - Configuration file parser for ModemBridge
 */

#include "config.h"
#include <strings.h>

/* Baudrate mapping table */
static const struct {
    int value;
    speed_t speed;
} baudrate_map[] = {
    {300, B300},
    {1200, B1200},
    {2400, B2400},
    {4800, B4800},
    {9600, B9600},
    {19200, B19200},
    {38400, B38400},
    {57600, B57600},
    {115200, B115200},
    {230400, B230400},
    {0, B0}
};

/**
 * Initialize configuration with default values
 */
void config_init(config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(config_t));

    /* Default serial port settings */
    SAFE_STRNCPY(cfg->serial_port, "/dev/ttyUSB0", sizeof(cfg->serial_port));
    cfg->baudrate = B57600;
    cfg->baudrate_value = 57600;
    cfg->parity = PARITY_NONE;
    cfg->data_bits = 8;
    cfg->stop_bits = 1;
    cfg->flow_control = FLOW_RTSCTS;

    /* Default telnet settings */
    SAFE_STRNCPY(cfg->telnet_host, "127.0.0.1", sizeof(cfg->telnet_host));
    cfg->telnet_port = 23;

    /* Default runtime options */
    cfg->daemon_mode = false;
    SAFE_STRNCPY(cfg->pid_file, DEFAULT_PID_FILE, sizeof(cfg->pid_file));
    cfg->log_level = LOG_INFO;

    /* Default data logging options */
    cfg->data_log_enabled = false;
    SAFE_STRNCPY(cfg->data_log_file, DEFAULT_DATALOG_FILE, sizeof(cfg->data_log_file));

    MB_LOG_DEBUG("Configuration initialized with defaults");
}

/**
 * Parse a single line from config file
 */
static int parse_config_line(config_t *cfg, char *line)
{
    char *key, *value, *equals;

    /* Remove comments */
    char *comment = strchr(line, '#');
    if (comment) {
        *comment = '\0';
    }

    /* Trim whitespace */
    line = trim_whitespace(line);

    /* Skip empty lines */
    if (strlen(line) == 0) {
        return SUCCESS;
    }

    /* Find equals sign */
    equals = strchr(line, '=');
    if (equals == NULL) {
        MB_LOG_WARNING("Invalid config line (no '=' found): %s", line);
        return ERROR_CONFIG;
    }

    /* Split into key and value */
    *equals = '\0';
    key = trim_whitespace(line);
    value = trim_whitespace(equals + 1);

    /* Remove quotes from value */
    if (value[0] == '"' && value[strlen(value) - 1] == '"') {
        value[strlen(value) - 1] = '\0';
        value++;
    }

    MB_LOG_DEBUG("Config: %s = %s", key, value);

    /* Parse key-value pairs */
    if (strcasecmp(key, "SERIAL_PORT") == 0 || strcasecmp(key, "COMPORT") == 0) {
        SAFE_STRNCPY(cfg->serial_port, value, sizeof(cfg->serial_port));
    }
    else if (strcasecmp(key, "BAUDRATE") == 0) {
        int baud = atoi(value);
        cfg->baudrate = config_baudrate_to_speed(baud);
        cfg->baudrate_value = baud;
        if (cfg->baudrate == B0 && baud != 0) {
            MB_LOG_WARNING("Unsupported baudrate: %d, using default", baud);
            cfg->baudrate = B57600;
            cfg->baudrate_value = 57600;
        }
    }
    else if (strcasecmp(key, "BIT_PARITY") == 0) {
        cfg->parity = config_str_to_parity(value);
    }
    else if (strcasecmp(key, "BIT_DATA") == 0) {
        cfg->data_bits = atoi(value);
        if (cfg->data_bits != 7 && cfg->data_bits != 8) {
            MB_LOG_WARNING("Invalid data bits: %d, using 8", cfg->data_bits);
            cfg->data_bits = 8;
        }
    }
    else if (strcasecmp(key, "BIT_STOP") == 0) {
        cfg->stop_bits = atoi(value);
        if (cfg->stop_bits != 1 && cfg->stop_bits != 2) {
            MB_LOG_WARNING("Invalid stop bits: %d, using 1", cfg->stop_bits);
            cfg->stop_bits = 1;
        }
    }
    else if (strcasecmp(key, "FLOW") == 0) {
        cfg->flow_control = config_str_to_flow(value);
    }
    else if (strcasecmp(key, "TELNET_HOST") == 0) {
        SAFE_STRNCPY(cfg->telnet_host, value, sizeof(cfg->telnet_host));
    }
    else if (strcasecmp(key, "TELNET_PORT") == 0) {
        cfg->telnet_port = atoi(value);
    }
    else if (strcasecmp(key, "DATA_LOG_ENABLED") == 0) {
        cfg->data_log_enabled = (atoi(value) != 0);
    }
    else if (strcasecmp(key, "DATA_LOG_FILE") == 0) {
        SAFE_STRNCPY(cfg->data_log_file, value, sizeof(cfg->data_log_file));
    }
    else {
        MB_LOG_WARNING("Unknown config key: %s", key);
    }

    return SUCCESS;
}

/**
 * Load configuration from file
 */
int config_load(config_t *cfg, const char *config_file)
{
    FILE *fp;
    char line[LINE_BUFFER_SIZE];
    int line_num = 0;

    if (cfg == NULL || config_file == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Loading configuration from: %s", config_file);

    fp = fopen(config_file, "r");
    if (fp == NULL) {
        MB_LOG_ERROR("Failed to open config file %s: %s", config_file, strerror(errno));
        return ERROR_CONFIG;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        line_num++;

        /* Remove newline */
        line[strcspn(line, "\r\n")] = '\0';

        if (parse_config_line(cfg, line) != SUCCESS) {
            MB_LOG_WARNING("Error parsing line %d: %s", line_num, line);
        }
    }

    fclose(fp);

    MB_LOG_INFO("Configuration loaded successfully");

    return SUCCESS;
}

/**
 * Validate configuration values
 */
int config_validate(const config_t *cfg)
{
    if (cfg == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Validate serial port */
    if (strlen(cfg->serial_port) == 0) {
        MB_LOG_ERROR("Serial port not specified");
        return ERROR_CONFIG;
    }

    /* Validate baudrate */
    if (cfg->baudrate == B0) {
        MB_LOG_ERROR("Invalid baudrate");
        return ERROR_CONFIG;
    }

    /* Validate data bits */
    if (cfg->data_bits != 7 && cfg->data_bits != 8) {
        MB_LOG_ERROR("Invalid data bits: %d (must be 7 or 8)", cfg->data_bits);
        return ERROR_CONFIG;
    }

    /* Validate stop bits */
    if (cfg->stop_bits != 1 && cfg->stop_bits != 2) {
        MB_LOG_ERROR("Invalid stop bits: %d (must be 1 or 2)", cfg->stop_bits);
        return ERROR_CONFIG;
    }

    /* Validate telnet host */
    if (strlen(cfg->telnet_host) == 0) {
        MB_LOG_ERROR("Telnet host not specified");
        return ERROR_CONFIG;
    }

    /* Validate telnet port */
    if (cfg->telnet_port <= 0 || cfg->telnet_port > 65535) {
        MB_LOG_ERROR("Invalid telnet port: %d", cfg->telnet_port);
        return ERROR_CONFIG;
    }

    MB_LOG_INFO("Configuration validated successfully");

    return SUCCESS;
}

/**
 * Print configuration to log and stdout
 */
void config_print(const config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    /* Print to stdout */
    printf("=== Configuration ===\n");
    printf("Serial Port:\n");
    printf("  Device:     %s\n", cfg->serial_port);
    printf("  Baudrate:   %d\n", cfg->baudrate_value);
    printf("  Parity:     %s\n", config_parity_to_str(cfg->parity));
    printf("  Data bits:  %d\n", cfg->data_bits);
    printf("  Stop bits:  %d\n", cfg->stop_bits);
    printf("  Flow ctrl:  %s\n", config_flow_to_str(cfg->flow_control));
    printf("Telnet:\n");
    printf("  Host:       %s\n", cfg->telnet_host);
    printf("  Port:       %d\n", cfg->telnet_port);
    printf("Data Logging:\n");
    printf("  Enabled:    %s\n", cfg->data_log_enabled ? "yes" : "no");
    printf("  File:       %s\n", cfg->data_log_file);
    printf("====================\n");

    /* Also log to syslog */
    MB_LOG_INFO("=== Configuration ===");
    MB_LOG_INFO("Serial Port:");
    MB_LOG_INFO("  Device:     %s", cfg->serial_port);
    MB_LOG_INFO("  Baudrate:   %d", cfg->baudrate_value);
    MB_LOG_INFO("  Parity:     %s", config_parity_to_str(cfg->parity));
    MB_LOG_INFO("  Data bits:  %d", cfg->data_bits);
    MB_LOG_INFO("  Stop bits:  %d", cfg->stop_bits);
    MB_LOG_INFO("  Flow ctrl:  %s", config_flow_to_str(cfg->flow_control));
    MB_LOG_INFO("Telnet:");
    MB_LOG_INFO("  Host:       %s", cfg->telnet_host);
    MB_LOG_INFO("  Port:       %d", cfg->telnet_port);
    MB_LOG_INFO("Data Logging:");
    MB_LOG_INFO("  Enabled:    %s", cfg->data_log_enabled ? "yes" : "no");
    MB_LOG_INFO("  File:       %s", cfg->data_log_file);
    MB_LOG_INFO("====================");
}

/**
 * Free configuration resources
 */
void config_free(config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    /* Nothing to free for now, but provided for future use */
}

/**
 * Convert baudrate value to termios speed_t
 */
speed_t config_baudrate_to_speed(int baudrate)
{
    for (size_t i = 0; baudrate_map[i].value != 0; i++) {
        if (baudrate_map[i].value == baudrate) {
            return baudrate_map[i].speed;
        }
    }

    MB_LOG_WARNING("Unsupported baudrate: %d", baudrate);
    return B0;
}

/**
 * Convert string to parity enum
 */
parity_t config_str_to_parity(const char *str)
{
    if (strcasecmp(str, "NONE") == 0) {
        return PARITY_NONE;
    } else if (strcasecmp(str, "EVEN") == 0) {
        return PARITY_EVEN;
    } else if (strcasecmp(str, "ODD") == 0) {
        return PARITY_ODD;
    }

    MB_LOG_WARNING("Unknown parity: %s, using NONE", str);
    return PARITY_NONE;
}

/**
 * Convert string to flow control enum
 */
flow_control_t config_str_to_flow(const char *str)
{
    if (strcasecmp(str, "NONE") == 0) {
        return FLOW_NONE;
    } else if (strcasecmp(str, "XON/XOFF") == 0) {
        return FLOW_XONXOFF;
    } else if (strcasecmp(str, "RTS/CTS") == 0) {
        return FLOW_RTSCTS;
    } else if (strcasecmp(str, "BOTH") == 0) {
        return FLOW_BOTH;
    }

    MB_LOG_WARNING("Unknown flow control: %s, using NONE", str);
    return FLOW_NONE;
}

/**
 * Convert parity enum to string
 */
const char *config_parity_to_str(parity_t parity)
{
    switch (parity) {
        case PARITY_NONE: return "NONE";
        case PARITY_EVEN: return "EVEN";
        case PARITY_ODD:  return "ODD";
        default:          return "UNKNOWN";
    }
}

/**
 * Convert flow control enum to string
 */
const char *config_flow_to_str(flow_control_t flow)
{
    switch (flow) {
        case FLOW_NONE:    return "NONE";
        case FLOW_XONXOFF: return "XON/XOFF";
        case FLOW_RTSCTS:  return "RTS/CTS";
        case FLOW_BOTH:    return "BOTH";
        default:           return "UNKNOWN";
    }
}
