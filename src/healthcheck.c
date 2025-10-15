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

/**
 * Run complete health check
 */
int healthcheck_run(const config_t *cfg, health_report_t *report)
{
    if (cfg == NULL || report == NULL) {
        return ERROR_INVALID_ARG;
    }

    memset(report, 0, sizeof(health_report_t));

    /* Check serial port */
    healthcheck_serial_port(cfg->serial_port, &report->serial_port);

    /* Check modem device (only if serial port is accessible) */
    if (report->serial_port.status == HEALTH_STATUS_OK) {
        healthcheck_modem_device(cfg->serial_port, cfg, &report->modem_device);
    } else {
        report->modem_device.status = HEALTH_STATUS_ERROR;
        SAFE_STRNCPY(report->modem_device.message,
                    "Cannot check modem (serial port not available)",
                    sizeof(report->modem_device.message));
    }

    /* Check telnet server */
    healthcheck_telnet_server(cfg->telnet_host, cfg->telnet_port,
                             &report->telnet_server);

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
 * Check modem device responsiveness
 * Sends AT command and waits for response (2 second timeout)
 */
int healthcheck_modem_device(const char *device, const config_t *cfg,
                             health_check_result_t *result)
{
    serial_port_t port;
    unsigned char response[SMALL_BUFFER_SIZE];
    const char *at_cmd = "AT\r\n";
    struct timeval tv;
    fd_set readfds;
    int ret;

    if (device == NULL || cfg == NULL || result == NULL) {
        return ERROR_INVALID_ARG;
    }

    result->status = HEALTH_STATUS_UNKNOWN;
    memset(result->message, 0, sizeof(result->message));

    /* Temporarily open serial port */
    serial_init(&port);
    if (serial_open(&port, device, cfg) != SUCCESS) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Cannot open serial port for modem check");
        return SUCCESS;
    }

    /* Send AT command */
    ssize_t written = serial_write(&port, (const unsigned char *)at_cmd, strlen(at_cmd));
    if (written < 0) {
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Failed to write AT command");
        serial_close(&port);
        return SUCCESS;
    }

    /* Wait for response (2 second timeout) */
    FD_ZERO(&readfds);
    FD_SET(port.fd, &readfds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    ret = select(port.fd + 1, &readfds, NULL, NULL, &tv);

    if (ret > 0) {
        /* Data available */
        ssize_t n = serial_read(&port, response, sizeof(response) - 1);
        if (n > 0) {
            result->status = HEALTH_STATUS_OK;
            snprintf(result->message, sizeof(result->message),
                    "Modem responded to AT command");
        } else {
            result->status = HEALTH_STATUS_WARNING;
            snprintf(result->message, sizeof(result->message),
                    "Read error from modem");
        }
    } else if (ret == 0) {
        /* Timeout */
        result->status = HEALTH_STATUS_WARNING;
        snprintf(result->message, sizeof(result->message),
                "No response from modem (timeout 2s) - modem may be offline");
    } else {
        /* Select error */
        result->status = HEALTH_STATUS_ERROR;
        snprintf(result->message, sizeof(result->message),
                "Select error: %s", strerror(errno));
    }

    serial_close(&port);
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
void healthcheck_print_report(const health_report_t *report)
{
    if (report == NULL) {
        return;
    }

    printf("=== Health Check ===\n");
    printf("\n");

    printf("Serial Port:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->serial_port.status));
    printf("  %s\n", report->serial_port.message);
    printf("\n");

    printf("Modem Device:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->modem_device.status));
    printf("  %s\n", report->modem_device.message);
    printf("\n");

    printf("Telnet Server:\n");
    printf("  Status: %s\n", healthcheck_status_to_str(report->telnet_server.status));
    printf("  %s\n", report->telnet_server.message);

    printf("====================\n");
}
