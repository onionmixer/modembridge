/*
 * healthcheck.c - Health check implementation for ModemBridge
 */

#include "healthcheck.h"
#include "serial.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* Forward declarations */
static int healthcheck_modem_device_with_port(serial_port_t *port, const config_t *cfg,
                                               health_check_result_t *result);
static int healthcheck_execute_init_commands(serial_port_t *port, const config_t *cfg,
                                              health_report_t *report);
static int healthcheck_execute_health_commands(serial_port_t *port, const config_t *cfg,
                                                health_report_t *report);

/**
 * Run complete health check
 */
int healthcheck_run(const config_t *cfg, health_report_t *report)
{
    serial_port_t port;
    bool port_opened = false;

    if (cfg == NULL || report == NULL) {
        return ERROR_INVALID_ARG;
    }

    memset(report, 0, sizeof(health_report_t));

    /* Check serial port (file existence and permissions only) */
    healthcheck_serial_port(cfg->serial_port, &report->serial_port);

    /* Open serial port once for the entire health check */
    if (report->serial_port.status == HEALTH_STATUS_OK) {
        serial_init(&port);
        if (serial_open(&port, cfg->serial_port, cfg) == SUCCESS) {
            port_opened = true;
            report->serial_init.status = HEALTH_STATUS_OK;
            snprintf(report->serial_init.message, sizeof(report->serial_init.message),
                    "Serial port initialized: %d baud, %d%c%d, flow=%s",
                    cfg->baudrate_value,
                    cfg->data_bits,
                    cfg->parity == PARITY_NONE ? 'N' :
                    cfg->parity == PARITY_EVEN ? 'E' : 'O',
                    cfg->stop_bits,
                    config_flow_to_str(cfg->flow_control));
        } else {
            report->serial_init.status = HEALTH_STATUS_ERROR;
            SAFE_STRNCPY(report->serial_init.message,
                        "Failed to initialize serial port",
                        sizeof(report->serial_init.message));
        }
    } else {
        report->serial_init.status = HEALTH_STATUS_ERROR;
        SAFE_STRNCPY(report->serial_init.message,
                    "Cannot initialize (serial port not available)",
                    sizeof(report->serial_init.message));
    }

    /* Check modem device (use already-open port) */
    if (port_opened) {
        healthcheck_modem_device_with_port(&port, cfg, &report->modem_device);
    } else {
        report->modem_device.status = HEALTH_STATUS_ERROR;
        SAFE_STRNCPY(report->modem_device.message,
                    "Cannot check modem (serial not initialized)",
                    sizeof(report->modem_device.message));
    }

    /* Execute MODEM_INIT_COMMAND if port is open and modem is accessible
     * NOTE: This executes and prints immediately, appearing in output
     * before the full report is printed by healthcheck_print_report() */
    if (port_opened && cfg->modem_init_command[0] != '\0' &&
        (report->modem_device.status == HEALTH_STATUS_OK ||
         report->modem_device.status == HEALTH_STATUS_WARNING)) {
        healthcheck_execute_init_commands(&port, cfg, report);
    }

    /* Execute MODEM_HEALTH_COMMAND if configured and port is open */
    if (port_opened && cfg->modem_command[0] != '\0' &&
        (report->modem_device.status == HEALTH_STATUS_OK ||
         report->modem_device.status == HEALTH_STATUS_WARNING)) {
        healthcheck_execute_health_commands(&port, cfg, report);
    }

    /* Close serial port once at the end */
    if (port_opened) {
        serial_close(&port);
    }

    /* === LEVEL 1: Telnet check DISABLED === */
    /* Level 1 only uses serial/modem, telnet check not needed */
    /* healthcheck_telnet_server(cfg->telnet_host, cfg->telnet_port,
                             &report->telnet_server); */

    /* Set telnet status to UNKNOWN since we're not checking it */
    report->telnet_server.status = HEALTH_STATUS_UNKNOWN;
    SAFE_STRNCPY(report->telnet_server.message,
                "Telnet check skipped (Level 1 mode)",
                sizeof(report->telnet_server.message));

    return SUCCESS;
}

/**
 * Check serial port availability
 */
int healthcheck_serial_port(const char *device, health_check_result_t *result)
{
    struct stat st;
    int fd;

    if (device == NULL || result == NULL) {
        return ERROR_INVALID_ARG;
    }

    result->status = HEALTH_STATUS_UNKNOWN;
    memset(result->message, 0, sizeof(result->message));

    /* 1. Check if device file exists */
    if (access(device, F_OK) != 0) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Device does not exist: %s", device);
        return SUCCESS;
    }

    /* 2. Check if it's a character device */
    if (stat(device, &st) != 0) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Cannot stat device: %s (%s)", device, strerror(errno));
        return SUCCESS;
    }

    if (!S_ISCHR(st.st_mode)) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Not a character device: %s", device);
        return SUCCESS;
    }

    /* 3. Check read/write permissions */
    if (access(device, R_OK | W_OK) != 0) {
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "Permission denied: %s (try: sudo usermod -a -G dialout $USER)",
                device);
        return SUCCESS;
    }

    /* 4. Try to open the device */
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "Failed to open: %s (%s)", device, strerror(errno));
        return SUCCESS;
    }

    close(fd);

    /* Success */
    result->status = HEALTH_STATUS_OK;
    snprintf(result->message, sizeof(result->message),
            "Device exists and accessible: %s", device);

    return SUCCESS;
}

/**
 * Helper: Send command to modem and read response with timeout
 * Reads all available data from modem by continuously reading until no more data
 * Returns total bytes read, or -1 on error, 0 on timeout
 */
static ssize_t send_at_command_and_wait(serial_port_t *port,
                                         const char *command,
                                         unsigned char *response,
                                         size_t response_size,
                                         int timeout_sec)
{
    char cmd_buf[SMALL_BUFFER_SIZE];
    fd_set readfds;
    struct timeval tv;
    int ret;
    ssize_t total_read = 0;
    unsigned char temp_buf[SMALL_BUFFER_SIZE];

    /* Format command with CR+LF */
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", command);

    /* Send command */
    ssize_t written = serial_write(port, (const unsigned char *)cmd_buf,
                                   strlen(cmd_buf));
    if (written < 0) {
        return -1;
    }

    /* Read response until no more data or timeout */
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(port->fd, &readfds);
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        ret = select(port->fd + 1, &readfds, NULL, NULL, &tv);

        if (ret > 0) {
            /* Data available - read it */
            ssize_t n = serial_read(port, temp_buf, sizeof(temp_buf));
            if (n > 0) {
                /* Append to response buffer if space available */
                if (total_read + n < (ssize_t)response_size - 1) {
                    memcpy(response + total_read, temp_buf, n);
                    total_read += n;
                } else {
                    /* Buffer full - copy what fits */
                    ssize_t space_left = response_size - 1 - total_read;
                    if (space_left > 0) {
                        memcpy(response + total_read, temp_buf, space_left);
                        total_read += space_left;
                    }
                    break;
                }

                /* Continue reading with shorter timeout (100ms) to catch remaining data */
                tv.tv_sec = 0;
                tv.tv_usec = 100000;
            } else if (n < 0) {
                /* Read error */
                if (total_read > 0) {
                    /* Return what we've read so far */
                    break;
                }
                return -1;
            } else {
                /* n == 0, no data available */
                break;
            }
        } else if (ret == 0) {
            /* Timeout */
            if (total_read > 0) {
                /* We got some data before timeout */
                break;
            }
            return 0;
        } else {
            /* Select error */
            if (total_read > 0) {
                /* Return what we've read so far */
                break;
            }
            return -1;
        }
    }

    if (total_read > 0) {
        response[total_read] = '\0';  /* Null-terminate */
    }

    return total_read;
}

/**
 * Helper: Parse semicolon-separated commands
 * Modifies input string in place
 * Returns number of commands parsed
 */
static int parse_modem_commands(char *modem_command_str,
                                char **commands,
                                int max_commands)
{
    int count = 0;
    char *token;
    char *saveptr;

    if (modem_command_str == NULL || modem_command_str[0] == '\0') {
        return 0;
    }

    /* Split by semicolon */
    token = strtok_r(modem_command_str, ";", &saveptr);
    while (token != NULL && count < max_commands) {
        /* Trim whitespace */
        token = trim_whitespace(token);

        if (strlen(token) > 0) {
            commands[count++] = token;
        }

        token = strtok_r(NULL, ";", &saveptr);
    }

    return count;
}

/**
 * Check modem device responsiveness using an already-open port
 * Sends AT command and waits for response (2 second timeout)
 */
static int healthcheck_modem_device_with_port(serial_port_t *port, const config_t *cfg,
                                               health_check_result_t *result)
{
    unsigned char response[SMALL_BUFFER_SIZE];

    if (port == NULL || cfg == NULL || result == NULL) {
        return ERROR_INVALID_ARG;
    }

    result->status = HEALTH_STATUS_UNKNOWN;
    memset(result->message, 0, sizeof(result->message));

    /* Send AT command and check response (silent check) */
    ssize_t n = send_at_command_and_wait(port, "AT", response,
                                         sizeof(response), 2);

    if (n > 0) {
        result->status = HEALTH_STATUS_OK;
        snprintf(result->message, sizeof(result->message),
                "Modem responded to AT command");
    } else if (n == 0) {
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "No response from modem (timeout 2s) - modem may be offline");
    } else {
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "Read error from modem");
    }

    return SUCCESS;
}

/**
 * Execute MODEM_INIT_COMMAND on an already-open port
 * Prints output directly (called during health check while port is open)
 */
static int healthcheck_execute_init_commands(serial_port_t *port, const config_t *cfg,
                                              health_report_t *report)
{
    unsigned char response[SMALL_BUFFER_SIZE];
    char modem_cmd_copy[LINE_BUFFER_SIZE];
    char *commands[32];
    int cmd_count = 0;

    (void)report;  /* Not used in this implementation */

    if (port == NULL || cfg == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\n  === Modem Init Command Execution ===\n");
    printf("  Sending: AT\n");

    ssize_t n = send_at_command_and_wait(port, "AT", response,
                                         sizeof(response), 2);

    if (n > 0) {
        printf("  Response (%zd bytes): ", n);
        printf("[HEX: ");
        for (ssize_t i = 0; i < n; i++) {
            printf("%02X ", response[i]);
        }
        printf("] [ASCII: ");
        int printed = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (response[i] >= 0x20 && response[i] <= 0x7E) {
                putchar(response[i]);
                printed = 1;
            }
        }
        if (!printed) printf("(only control chars)");
        printf("]\n");
    } else if (n == 0) {
        printf("  Response: (timeout)\n");
    } else {
        printf("  Response: (error)\n");
    }

    /* Execute MODEM_INIT_COMMAND if configured */
    if (cfg->modem_init_command[0] != '\0') {
        printf("\n  --- Executing MODEM_INIT_COMMAND ---\n");
        printf("  Raw MODEM_INIT_COMMAND: %s\n\n", cfg->modem_init_command);

        SAFE_STRNCPY(modem_cmd_copy, cfg->modem_init_command,
                   sizeof(modem_cmd_copy));
        cmd_count = parse_modem_commands(modem_cmd_copy, commands, 32);

        for (int i = 0; i < cmd_count; i++) {
            /* Add 2-second delay between commands (except before first command) */
            if (i > 0) {
                sleep(2);
            }

            printf("  Command %d/%d: %s\n", i + 1, cmd_count, commands[i]);

            memset(response, 0, sizeof(response));
            n = send_at_command_and_wait(port, commands[i], response,
                                         sizeof(response), 2);

            if (n > 0) {
                printf("  Response (%zd bytes): ", n);
                printf("[HEX: ");
                for (ssize_t j = 0; j < n; j++) {
                    printf("%02X ", response[j]);
                }
                printf("] [ASCII: ");
                int printed = 0;
                for (ssize_t j = 0; j < n; j++) {
                    if (response[j] >= 0x20 && response[j] <= 0x7E) {
                        putchar(response[j]);
                        printed = 1;
                    }
                }
                if (!printed) printf("(only control chars)");
                printf("]\n\n");
            } else if (n == 0) {
                printf("  Response: (timeout)\n\n");
            } else {
                printf("  Response: (error)\n\n");
            }
        }

        printf("  Total commands sent: %d\n", cmd_count);
    } else {
        printf("\n  MODEM_INIT_COMMAND: (not configured)\n");
    }

    printf("  ================================\n");

    return SUCCESS;
}

/**
 * Execute MODEM_HEALTH_COMMAND on an already-open port
 * Prints output directly (called during health check while port is open)
 */
static int healthcheck_execute_health_commands(serial_port_t *port, const config_t *cfg,
                                                health_report_t *report)
{
    unsigned char response[SMALL_BUFFER_SIZE];
    char modem_cmd_copy[LINE_BUFFER_SIZE];
    char *commands[32];
    int cmd_count = 0;

    (void)report;  /* Not used in this implementation */

    if (port == NULL || cfg == NULL) {
        return ERROR_INVALID_ARG;
    }

    printf("\n  === Modem Health Command Execution ===\n");

    /* Execute MODEM_HEALTH_COMMAND if configured */
    if (cfg->modem_command[0] != '\0') {
        printf("  Raw MODEM_HEALTH_COMMAND: %s\n\n", cfg->modem_command);

        SAFE_STRNCPY(modem_cmd_copy, cfg->modem_command,
                   sizeof(modem_cmd_copy));
        cmd_count = parse_modem_commands(modem_cmd_copy, commands, 32);

        for (int i = 0; i < cmd_count; i++) {
            /* Add 1-second delay between commands (except before first command) */
            if (i > 0) {
                sleep(1);
            }

            printf("  Command %d/%d: %s\n", i + 1, cmd_count, commands[i]);

            memset(response, 0, sizeof(response));
            ssize_t n = send_at_command_and_wait(port, commands[i], response,
                                         sizeof(response), 2);

            if (n > 0) {
                printf("  Response (%zd bytes): ", n);
                printf("[HEX: ");
                for (ssize_t j = 0; j < n; j++) {
                    printf("%02X ", response[j]);
                }
                printf("] [ASCII: ");
                int printed = 0;
                for (ssize_t j = 0; j < n; j++) {
                    if (response[j] >= 0x20 && response[j] <= 0x7E) {
                        putchar(response[j]);
                        printed = 1;
                    }
                }
                if (!printed) printf("(only control chars)");
                printf("]\n\n");
            } else if (n == 0) {
                printf("  Response: (timeout)\n\n");
            } else {
                printf("  Response: (error)\n\n");
            }
        }

        printf("  Total health commands sent: %d\n", cmd_count);
    } else {
        printf("  MODEM_HEALTH_COMMAND: (not configured)\n");
    }

    printf("  ====================================\n");

    return SUCCESS;
}

/**
 * Check telnet server connectivity
 * Attempts TCP connection with 5 second timeout
 */
int healthcheck_telnet_server(const char *host, int port,
                              health_check_result_t *result)
{
    int sockfd;
    struct sockaddr_in server_addr;
    struct hostent *he;
    struct timeval tv;
    fd_set writefds;
    int ret;
    int flags;

    if (host == NULL || result == NULL) {
        return ERROR_INVALID_ARG;
    }

    result->status = HEALTH_STATUS_UNKNOWN;
    memset(result->message, 0, sizeof(result->message));

    /* DNS resolution */
    he = gethostbyname(host);
    if (he == NULL) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Failed to resolve hostname: %s", host);
        return SUCCESS;
    }

    /* Create socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Failed to create socket: %s", strerror(errno));
        return SUCCESS;
    }

    /* Set non-blocking mode */
    flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    /* Prepare server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    /* Attempt connection */
    ret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    if (ret == 0) {
        /* Immediate connection (localhost) */
        result->status = HEALTH_STATUS_OK;
        snprintf(result->message, sizeof(result->message),
                "Connected: %s:%d", host, port);
        close(sockfd);
        return SUCCESS;
    }

    if (errno != EINPROGRESS) {
        /* Connection failed immediately */
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Connection failed: %s:%d (%s)", host, port, strerror(errno));
        close(sockfd);
        return SUCCESS;
    }

    /* Wait for connection to complete (5 second timeout) */
    FD_ZERO(&writefds);
    FD_SET(sockfd, &writefds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    ret = select(sockfd + 1, NULL, &writefds, NULL, &tv);

    if (ret > 0) {
        /* Check if connection succeeded */
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);

        if (error == 0) {
            result->status = HEALTH_STATUS_OK;
            snprintf(result->message, sizeof(result->message),
                    "Connected: %s:%d", host, port);
        } else {
            result->status = HEALTH_STATUS_ERROR;
            snprintf(result->message, sizeof(result->message),
                    "Connection failed: %s:%d (%s)", host, port, strerror(error));
        }
    } else if (ret == 0) {
        /* Timeout */
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "Connection timeout (5s): %s:%d", host, port);
    } else {
        /* Select error */
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Select error: %s", strerror(errno));
    }

    close(sockfd);
    return SUCCESS;
}

/**
 * Convert health status to string
 */
const char *healthcheck_status_to_str(health_status_t status)
{
    switch (status) {
        case HEALTH_STATUS_OK:      return "OK";
        case HEALTH_STATUS_WARNING: return "WARNING";
        case HEALTH_STATUS_ERROR:   return "ERROR";
        case HEALTH_STATUS_UNKNOWN: return "UNKNOWN";
        default:                    return "INVALID";
    }
}

/**
 * Print health check report
 */
void healthcheck_print_report(const health_report_t *report, const config_t *cfg)
{
    (void)cfg;  /* cfg parameter no longer used - kept for API compatibility */

    if (report == NULL) {
        return;
    }

    printf("=== Health Check ===\n");
    printf("\n");

    printf("Serial Port:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->serial_port.status));
    printf("  %s\n", report->serial_port.message);
    printf("\n");

    printf("Serial Initialization:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->serial_init.status));
    printf("  %s\n", report->serial_init.message);
    printf("\n");

    printf("Modem Device:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->modem_device.status));
    printf("  %s\n", report->modem_device.message);

    /* MODEM_INIT_COMMAND execution has already been done in healthcheck_run()
     * while the serial port was open - no need to reopen here */

    printf("\n");

    /* === LEVEL 1: Telnet output DISABLED === */
    /* printf("Telnet Server:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->telnet_server.status));
    printf("  %s\n", report->telnet_server.message);
    printf("\n"); */

    printf("====================\n");
}
