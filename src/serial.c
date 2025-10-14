/*
 * serial.c - Serial port communication implementation
 */

#include "serial.h"
#include <sys/ioctl.h>

/**
 * Initialize serial port structure
 */
void serial_init(serial_port_t *port)
{
    if (port == NULL) {
        return;
    }

    memset(port, 0, sizeof(serial_port_t));
    port->fd = -1;
    port->is_open = false;
}

/**
 * Open serial port with configuration
 */
int serial_open(serial_port_t *port, const char *device, const config_t *cfg)
{
    if (port == NULL || device == NULL || cfg == NULL) {
        MB_LOG_ERROR("Invalid arguments to serial_open");
        return ERROR_INVALID_ARG;
    }

    if (port->is_open) {
        MB_LOG_WARNING("Serial port already open, closing first");
        serial_close(port);
    }

    MB_LOG_INFO("Opening serial port: %s", device);

    /* Open serial device */
    port->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port->fd < 0) {
        MB_LOG_ERROR("Failed to open %s: %s", device, strerror(errno));
        return ERROR_IO;
    }

    /* Save device name */
    SAFE_STRNCPY(port->device, device, sizeof(port->device));

    /* Save original terminal settings */
    if (tcgetattr(port->fd, &port->oldtio) < 0) {
        MB_LOG_ERROR("Failed to get terminal attributes: %s", strerror(errno));
        close(port->fd);
        port->fd = -1;
        return ERROR_IO;
    }

    /* Configure serial port */
    int ret = serial_configure(port, cfg->baudrate, cfg->parity,
                               cfg->data_bits, cfg->stop_bits, cfg->flow_control);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to configure serial port");
        close(port->fd);
        port->fd = -1;
        return ret;
    }

    port->baudrate = cfg->baudrate;
    port->is_open = true;

    MB_LOG_INFO("Serial port opened successfully: %s", device);

    return SUCCESS;
}

/**
 * Close serial port
 */
int serial_close(serial_port_t *port)
{
    if (port == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || port->fd < 0) {
        return SUCCESS;
    }

    MB_LOG_INFO("Closing serial port: %s", port->device);

    /* Restore original terminal settings */
    tcsetattr(port->fd, TCSANOW, &port->oldtio);

    /* Close file descriptor */
    close(port->fd);
    port->fd = -1;
    port->is_open = false;

    MB_LOG_INFO("Serial port closed");

    return SUCCESS;
}

/**
 * Configure serial port parameters
 */
int serial_configure(serial_port_t *port, speed_t baudrate, parity_t parity,
                     int data_bits, int stop_bits, flow_control_t flow)
{
    struct termios newtio;

    if (port == NULL || port->fd < 0) {
        MB_LOG_ERROR("Invalid serial port");
        return ERROR_INVALID_ARG;
    }

    MB_LOG_DEBUG("Configuring serial port: baudrate=%d, parity=%d, data=%d, stop=%d, flow=%d",
                 baudrate, parity, data_bits, stop_bits, flow);

    /* Initialize termios structure */
    memset(&newtio, 0, sizeof(newtio));

    /* Control flags */
    newtio.c_cflag = CREAD | CLOCAL;  /* Enable receiver, ignore modem control lines */

    /* Set baudrate */
    cfsetispeed(&newtio, baudrate);
    cfsetospeed(&newtio, baudrate);

    /* Set data bits */
    switch (data_bits) {
        case 7:
            newtio.c_cflag |= CS7;
            break;
        case 8:
            newtio.c_cflag |= CS8;
            break;
        default:
            MB_LOG_ERROR("Invalid data bits: %d", data_bits);
            return ERROR_INVALID_ARG;
    }

    /* Set parity */
    switch (parity) {
        case PARITY_NONE:
            /* No parity */
            break;
        case PARITY_EVEN:
            newtio.c_cflag |= PARENB;
            break;
        case PARITY_ODD:
            newtio.c_cflag |= PARENB | PARODD;
            break;
        default:
            MB_LOG_ERROR("Invalid parity: %d", parity);
            return ERROR_INVALID_ARG;
    }

    /* Set stop bits */
    if (stop_bits == 2) {
        newtio.c_cflag |= CSTOPB;
    }
    /* 1 stop bit is default (no flag) */

    /* Set flow control */
    switch (flow) {
        case FLOW_NONE:
            /* No flow control */
            break;
        case FLOW_XONXOFF:
            newtio.c_iflag |= IXON | IXOFF;
            break;
        case FLOW_RTSCTS:
            newtio.c_cflag |= CRTSCTS;
            break;
        case FLOW_BOTH:
            newtio.c_iflag |= IXON | IXOFF;
            newtio.c_cflag |= CRTSCTS;
            break;
        default:
            MB_LOG_ERROR("Invalid flow control: %d", flow);
            return ERROR_INVALID_ARG;
    }

    /* Input flags - disable input processing */
    newtio.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP);

    /* Output flags - disable output processing (raw output) */
    newtio.c_oflag = 0;

    /* Local flags - raw mode, no echo */
    newtio.c_lflag = 0;

    /* Control characters - non-blocking read */
    newtio.c_cc[VMIN] = 0;   /* Non-blocking read */
    newtio.c_cc[VTIME] = 0;  /* No timeout */

    /* Apply settings */
    if (tcsetattr(port->fd, TCSANOW, &newtio) < 0) {
        MB_LOG_ERROR("Failed to set terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Flush buffers */
    tcflush(port->fd, TCIOFLUSH);

    /* Save new settings */
    memcpy(&port->newtio, &newtio, sizeof(newtio));

    MB_LOG_DEBUG("Serial port configured successfully");

    return SUCCESS;
}

/**
 * Change baudrate dynamically
 */
int serial_set_baudrate(serial_port_t *port, speed_t baudrate)
{
    struct termios tio;

    if (port == NULL || port->fd < 0) {
        MB_LOG_ERROR("Invalid serial port");
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Changing baudrate to %d", baudrate);

    /* Get current settings */
    if (tcgetattr(port->fd, &tio) < 0) {
        MB_LOG_ERROR("Failed to get terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Set new baudrate */
    cfsetispeed(&tio, baudrate);
    cfsetospeed(&tio, baudrate);

    /* Apply settings */
    if (tcsetattr(port->fd, TCSANOW, &tio) < 0) {
        MB_LOG_ERROR("Failed to set baudrate: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Flush buffers */
    tcflush(port->fd, TCIOFLUSH);

    port->baudrate = baudrate;

    MB_LOG_INFO("Baudrate changed successfully");

    return SUCCESS;
}

/**
 * Read data from serial port (non-blocking)
 */
ssize_t serial_read(serial_port_t *port, void *buffer, size_t size)
{
    ssize_t n;

    if (port == NULL || buffer == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    n = read(port->fd, buffer, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No data available (non-blocking) */
            return 0;
        }
        MB_LOG_ERROR("Serial read error: %s", strerror(errno));
        return ERROR_IO;
    }

    if (n > 0) {
        MB_LOG_DEBUG("Serial read: %zd bytes", n);
        hexdump("RX", buffer, n);
    }

    return n;
}

/**
 * Write data to serial port
 */
ssize_t serial_write(serial_port_t *port, const void *buffer, size_t size)
{
    ssize_t n;

    if (port == NULL || buffer == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Serial write: %zu bytes", size);
    hexdump("TX", buffer, size);

    n = write(port->fd, buffer, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Would block (shouldn't happen often on serial ports) */
            return 0;
        }
        MB_LOG_ERROR("Serial write error: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Wait for data to be transmitted */
    tcdrain(port->fd);

    return n;
}

/**
 * Flush serial port buffers
 */
int serial_flush(serial_port_t *port, int queue_selector)
{
    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    if (tcflush(port->fd, queue_selector) < 0) {
        MB_LOG_ERROR("Failed to flush serial buffers: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Serial buffers flushed");

    return SUCCESS;
}

/**
 * Set DTR (Data Terminal Ready) signal
 */
int serial_set_dtr(serial_port_t *port, bool state)
{
    int status;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (ioctl(port->fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("Failed to get modem status: %s", strerror(errno));
        return ERROR_IO;
    }

    if (state) {
        status |= TIOCM_DTR;
    } else {
        status &= ~TIOCM_DTR;
    }

    if (ioctl(port->fd, TIOCMSET, &status) < 0) {
        MB_LOG_ERROR("Failed to set DTR: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_DEBUG("DTR set to %s", state ? "ON" : "OFF");

    return SUCCESS;
}

/**
 * Set RTS (Request To Send) signal
 */
int serial_set_rts(serial_port_t *port, bool state)
{
    int status;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (ioctl(port->fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("Failed to get modem status: %s", strerror(errno));
        return ERROR_IO;
    }

    if (state) {
        status |= TIOCM_RTS;
    } else {
        status &= ~TIOCM_RTS;
    }

    if (ioctl(port->fd, TIOCMSET, &status) < 0) {
        MB_LOG_ERROR("Failed to set RTS: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_DEBUG("RTS set to %s", state ? "ON" : "OFF");

    return SUCCESS;
}

/**
 * Get DCD (Data Carrier Detect) signal state
 */
int serial_get_dcd(serial_port_t *port, bool *state)
{
    int status;

    if (port == NULL || state == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (ioctl(port->fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("Failed to get modem status: %s", strerror(errno));
        return ERROR_IO;
    }

    *state = (status & TIOCM_CAR) != 0;

    return SUCCESS;
}

/**
 * Get DSR (Data Set Ready) signal state
 */
int serial_get_dsr(serial_port_t *port, bool *state)
{
    int status;

    if (port == NULL || state == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (ioctl(port->fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("Failed to get modem status: %s", strerror(errno));
        return ERROR_IO;
    }

    *state = (status & TIOCM_DSR) != 0;

    return SUCCESS;
}

/**
 * Get CTS (Clear To Send) signal state
 */
int serial_get_cts(serial_port_t *port, bool *state)
{
    int status;

    if (port == NULL || state == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (ioctl(port->fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("Failed to get modem status: %s", strerror(errno));
        return ERROR_IO;
    }

    *state = (status & TIOCM_CTS) != 0;

    return SUCCESS;
}

/**
 * Get file descriptor for select/poll
 */
int serial_get_fd(serial_port_t *port)
{
    if (port == NULL) {
        return -1;
    }

    return port->fd;
}

/**
 * Check if serial port is open
 */
bool serial_is_open(serial_port_t *port)
{
    if (port == NULL) {
        return false;
    }

    return port->is_open && port->fd >= 0;
}
