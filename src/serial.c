/*
 * serial.c - Serial port communication implementation
 */

#include "serial.h"
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <time.h>

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
    port->epoll_fd = -1;
    port->is_open = false;

    /* Initialize Level 3 configuration */
    serial_init_level3_config(port);
}

/**
 * Open serial port with configuration
 * Enhanced with modem_sample pattern for reliability
 */
int serial_open(serial_port_t *port, const char *device, const config_t *cfg)
{
    int flags;

    if (port == NULL || device == NULL || cfg == NULL) {
        MB_LOG_ERROR("Invalid arguments to serial_open");
        return ERROR_INVALID_ARG;
    }

    if (port->is_open) {
        MB_LOG_WARNING("Serial port already open, closing first");
        serial_close(port);
    }

    MB_LOG_INFO("Opening serial port: %s", device);

    /* NOTE: Port locking should be done by caller BEFORE calling serial_open() */
    /* This matches modem_sample pattern where lock_port() is called first */

    /* Step 1: Open serial device with O_NOCTTY | O_NONBLOCK (modem_sample pattern) */
    port->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port->fd < 0) {
        MB_LOG_ERROR("Failed to open %s: %s", device, strerror(errno));
        return ERROR_IO;
    }

    /* Save device name */
    SAFE_STRNCPY(port->device, device, sizeof(port->device));

    /* Step 2: Save original terminal settings for restoration (modem_sample pattern) */
    if (tcgetattr(port->fd, &port->oldtio) < 0) {
        MB_LOG_ERROR("Failed to get terminal attributes: %s", strerror(errno));
        close(port->fd);
        port->fd = -1;
        return ERROR_IO;
    }

    /* Step 3: Configure serial port with TCSADRAIN (modem_sample pattern) */
    int ret = serial_configure(port, cfg->baudrate, cfg->parity,
                               cfg->data_bits, cfg->stop_bits, cfg->flow_control);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to configure serial port");
        close(port->fd);
        port->fd = -1;
        return ret;
    }

    /* Step 4: Convert to blocking mode AFTER configuration (modem_sample critical pattern) */
    flags = fcntl(port->fd, F_GETFL, 0);
    if (flags == -1) {
        MB_LOG_ERROR("Failed to get file flags: %s", strerror(errno));
        close(port->fd);
        port->fd = -1;
        return ERROR_IO;
    }

    flags &= ~O_NONBLOCK;  /* Clear non-blocking flag */
    if (fcntl(port->fd, F_SETFL, flags) == -1) {
        MB_LOG_ERROR("Failed to set blocking mode: %s", strerror(errno));
        close(port->fd);
        port->fd = -1;
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Serial port converted to blocking mode");

    /* Step 5: Create epoll instance for this serial port */
    port->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (port->epoll_fd < 0) {
        MB_LOG_ERROR("epoll_create1() failed: %s", strerror(errno));
        close(port->fd);
        port->fd = -1;
        return ERROR_IO;
    }

    /* Add serial port FD to epoll with read and error events */
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.fd = port->fd;

    if (epoll_ctl(port->epoll_fd, EPOLL_CTL_ADD, port->fd, &ev) < 0) {
        MB_LOG_ERROR("epoll_ctl(ADD) failed: %s", strerror(errno));
        close(port->epoll_fd);
        close(port->fd);
        port->epoll_fd = -1;
        port->fd = -1;
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Serial port epoll instance created (epoll_fd=%d)", port->epoll_fd);

    /* Step 6: Small delay for hardware stabilization (modem_sample pattern) */
    usleep(50000);  /* 50ms */

    port->baudrate = cfg->baudrate;
    port->is_open = true;

    MB_LOG_INFO("Serial port opened successfully: %s (locked, blocking mode, epoll_fd=%d)",
                device, port->epoll_fd);

    return SUCCESS;
}

/**
 * Close serial port
 * Enhanced with modem_sample pattern - includes port unlocking
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

    /* Drop DTR before closing to signal disconnect (modem_sample pattern) */
    if (serial_set_dtr(port, false) != SUCCESS) {
        MB_LOG_WARNING("Failed to drop DTR");
    }

    /* Restore original terminal settings */
    if (tcsetattr(port->fd, TCSANOW, &port->oldtio) < 0) {
        MB_LOG_WARNING("Failed to restore terminal settings: %s", strerror(errno));
        /* Continue closing anyway */
    }

    /* Close epoll instance */
    if (port->epoll_fd >= 0) {
        MB_LOG_DEBUG("Closing epoll instance (epoll_fd=%d)", port->epoll_fd);
        close(port->epoll_fd);
        port->epoll_fd = -1;
    }

    /* Close file descriptor */
    close(port->fd);
    port->fd = -1;
    port->is_open = false;

    /* NOTE: Port unlocking should be done by caller AFTER serial_close() */
    /* This matches modem_sample pattern where unlock_port() is called in cleanup */

    MB_LOG_INFO("Serial port closed");

    return SUCCESS;
}

/**
 * Configure serial port parameters
 * Enhanced with modem_sample pattern for optimal settings
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

    /* Start with saved terminal settings (modem_sample pattern) */
    newtio = port->oldtio;

    /* Input flags - NO input processing (modem_sample critical pattern) */
    newtio.c_iflag = 0;  /* Clear all input processing */

    /* Only add back flow control if requested */
    if (flow == FLOW_XONXOFF || flow == FLOW_BOTH) {
        newtio.c_iflag |= IXON | IXOFF;
    }

    /* Output flags - CR->CRLF conversion (modem_sample pattern from MBSE BBS) */
    newtio.c_oflag = OPOST | ONLCR;

    /* Control flags - modem_sample pattern */
    newtio.c_cflag = CREAD | CLOCAL | HUPCL;  /* Enable receiver, ignore carrier initially, hangup on close */

    /* Clear parity and stop bit settings first */
    newtio.c_cflag &= ~(CSTOPB | PARENB | PARODD);

    /* Set data bits */
    newtio.c_cflag &= ~CSIZE;  /* Clear size bits first */
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
            /* No parity - already cleared */
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

    /* Hardware flow control */
    if (flow == FLOW_RTSCTS || flow == FLOW_BOTH) {
        newtio.c_cflag |= CRTSCTS;
    }

    /* Local flags - raw mode, no echo (modem_sample pattern) */
    newtio.c_lflag = 0;

    /* Control characters - modem_sample pattern for blocking read */
    newtio.c_cc[VMIN] = 1;    /* Block until at least 1 byte (modem_sample pattern) */
    newtio.c_cc[VTIME] = 0;   /* No timeout between bytes */

    /* Set baudrate */
    cfsetispeed(&newtio, baudrate);
    cfsetospeed(&newtio, baudrate);

    /* Apply settings with TCSADRAIN - wait for output to complete (modem_sample pattern) */
    if (tcsetattr(port->fd, TCSADRAIN, &newtio) < 0) {
        MB_LOG_ERROR("Failed to set terminal attributes: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Flush buffers after configuration */
    tcflush(port->fd, TCIOFLUSH);

    /* Save new settings */
    memcpy(&port->newtio, &newtio, sizeof(newtio));

    /* Raise DTR to indicate terminal is ready */
    if (serial_set_dtr(port, true) != SUCCESS) {
        MB_LOG_WARNING("Failed to set DTR high");
    }

    /* Small delay for hardware to stabilize (modem_sample pattern) */
    usleep(50000);  /* 50ms */

    MB_LOG_DEBUG("Serial port configured successfully with modem_sample optimizations");

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
 * Read data from serial port with timeout (epoll-based)
 *
 * Uses epoll_wait() to prevent blocking, even when port is in blocking mode.
 * This allows draining loops and other callers to timeout gracefully.
 * Each serial port has its own epoll instance for independent I/O monitoring.
 *
 * Returns:
 *   > 0: Number of bytes read
 *   0: Timeout - no data available
 *   < 0: Error (ERROR_INVALID_ARG, ERROR_IO)
 */
ssize_t serial_read(serial_port_t *port, void *buffer, size_t size)
{
    struct epoll_event events[1];
    int nfds;
    ssize_t n;

    if (port == NULL || buffer == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || port->epoll_fd < 0) {
        return ERROR_IO;
    }

    /* Wait for read event with 100ms timeout */
    nfds = epoll_wait(port->epoll_fd, events, 1, 100);  /* 100ms timeout */

    if (nfds < 0) {
        /* epoll_wait() error */
        MB_LOG_ERROR("epoll_wait() failed: %s", strerror(errno));
        return ERROR_IO;
    } else if (nfds == 0) {
        /* Timeout - no data available */
        return 0;
    }

    /* Check for error or hangup events */
    if (events[0].events & (EPOLLERR | EPOLLHUP)) {
        MB_LOG_ERROR("Serial port error or hangup (events=0x%x)", events[0].events);
        return ERROR_IO;
    }

    /* Read data if EPOLLIN is set */
    if (events[0].events & EPOLLIN) {
        n = read(port->fd, buffer, size);

        if (n < 0) {
            /* Check for hangup conditions */
            if (errno == EPIPE || errno == ECONNRESET) {
                MB_LOG_ERROR("Serial port hangup during read");
                return ERROR_IO;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Shouldn't happen since epoll said readable, but handle anyway */
                return 0;
            }

            MB_LOG_ERROR("Serial read error: %s", strerror(errno));
            return ERROR_IO;
        }

        if (n > 0) {
            MB_LOG_DEBUG("Serial read: %zd bytes", n);
            hexdump("RX", buffer, n);

            /* Handle software flow control characters */
            serial_handle_flow_control(port, (const char *)buffer, n);
        }

        return n;
    }

    /* epoll returned event but no EPOLLIN - shouldn't happen */
    return 0;
}

/**
 * Write data to serial port
 */
ssize_t serial_write(serial_port_t *port, const void *buffer, size_t size)
{
    ssize_t n;

    if (port == NULL) {
        MB_LOG_ERROR("serial_write: port is NULL");
        return ERROR_INVALID_ARG;
    }
    if (buffer == NULL) {
        MB_LOG_ERROR("serial_write: buffer is NULL");
        return ERROR_INVALID_ARG;
    }
    if (port->fd < 0) {
        MB_LOG_ERROR("serial_write: invalid fd=%d", port->fd);
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        MB_LOG_ERROR("serial_write: port is not open");
        return ERROR_IO;
    }

    /* Check flow control before writing */
    if (serial_is_tx_blocked(port)) {
        MB_LOG_DEBUG("Serial write blocked by flow control");
        return 0;  /* Indicate no bytes written due to flow control */
    }

    MB_LOG_DEBUG("Serial write: %zu bytes to fd=%d", size, port->fd);
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
 * Write data to serial port using epoll for write readiness
 * Enhanced for timestamp transmission with partial write handling
 */
ssize_t serial_write_with_epoll(serial_port_t *port, const void *buffer, size_t size, int timeout_ms)
{
    struct epoll_event ev, events[1];
    int nfds;
    ssize_t total_written = 0;
    ssize_t n;
    const unsigned char *data = (const unsigned char *)buffer;

    if (port == NULL) {
        MB_LOG_ERROR("serial_write_with_epoll: port is NULL");
        return ERROR_INVALID_ARG;
    }
    if (buffer == NULL) {
        MB_LOG_ERROR("serial_write_with_epoll: buffer is NULL");
        return ERROR_INVALID_ARG;
    }
    if (port->fd < 0) {
        MB_LOG_ERROR("serial_write_with_epoll: invalid fd=%d", port->fd);
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || port->epoll_fd < 0) {
        MB_LOG_ERROR("serial_write_with_epoll: port is not open or epoll_fd invalid");
        return ERROR_IO;
    }

    MB_LOG_DEBUG("Serial write with epoll: %zu bytes to fd=%d (timeout=%dms)", size, port->fd, timeout_ms);

    /* Temporarily modify epoll to monitor EPOLLOUT */
    ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
    ev.data.fd = port->fd;

    if (epoll_ctl(port->epoll_fd, EPOLL_CTL_MOD, port->fd, &ev) < 0) {
        MB_LOG_ERROR("epoll_ctl(MOD for EPOLLOUT) failed: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Write loop with partial write handling */
    while (total_written < (ssize_t)size) {
        /* Wait for write readiness */
        nfds = epoll_wait(port->epoll_fd, events, 1, timeout_ms);

        if (nfds < 0) {
            MB_LOG_ERROR("epoll_wait() failed: %s", strerror(errno));
            /* Restore epoll to read-only before returning */
            ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
            epoll_ctl(port->epoll_fd, EPOLL_CTL_MOD, port->fd, &ev);
            return ERROR_IO;
        } else if (nfds == 0) {
            /* Timeout */
            MB_LOG_WARNING("Write timeout after %d bytes", (int)total_written);
            /* Restore epoll to read-only before returning */
            ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
            epoll_ctl(port->epoll_fd, EPOLL_CTL_MOD, port->fd, &ev);
            return (total_written > 0) ? total_written : ERROR_TIMEOUT;
        }

        /* Check for error or hangup events */
        if (events[0].events & (EPOLLERR | EPOLLHUP)) {
            MB_LOG_ERROR("Serial port error or hangup during write (events=0x%x)", events[0].events);
            /* Restore epoll to read-only before returning */
            ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
            epoll_ctl(port->epoll_fd, EPOLL_CTL_MOD, port->fd, &ev);
            return ERROR_IO;
        }

        /* Write data if EPOLLOUT is set */
        if (events[0].events & EPOLLOUT) {
            n = write(port->fd, data + total_written, size - total_written);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Would block - continue epoll loop */
                    continue;
                }
                MB_LOG_ERROR("Serial write error: %s", strerror(errno));
                /* Restore epoll to read-only before returning */
                ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                epoll_ctl(port->epoll_fd, EPOLL_CTL_MOD, port->fd, &ev);
                return ERROR_IO;
            }

            total_written += n;
            MB_LOG_DEBUG("Wrote %zd bytes (%zd/%zu total)", n, total_written, size);
        }
    }

    /* Restore epoll to read-only mode */
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    if (epoll_ctl(port->epoll_fd, EPOLL_CTL_MOD, port->fd, &ev) < 0) {
        MB_LOG_WARNING("epoll_ctl(MOD restore to read-only) failed: %s", strerror(errno));
        /* Not fatal - write succeeded */
    }

    /* Wait for data to be transmitted */
    tcdrain(port->fd);

    MB_LOG_DEBUG("Serial write with epoll completed: %zd bytes", total_written);
    return total_written;
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
int serial_get_dcd(serial_port_t *port)
{
    int status;

    if (port == NULL || port->fd < 0) {
        return -1;
    }

    if (ioctl(port->fd, TIOCMGET, &status) < 0) {
        MB_LOG_ERROR("Failed to get modem status: %s", strerror(errno));
        return -1;
    }

    return (status & TIOCM_CAR) ? 1 : 0;
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

/* ========================================================================
 * Extended functions from modem_sample
 * ======================================================================== */

/* Global state for port locking */
static char g_lock_file[256] = {0};

/**
 * Read a line from serial port (until \r or \n)
 * Based on modem_sample/serial_port.c:serial_read_line()
 */
ssize_t serial_read_line(serial_port_t *port, char *buffer, size_t size, int timeout_sec)
{
    static char read_buffer[512];     /* Internal buffer for accumulating data */
    static size_t buf_pos = 0;        /* Current position in read_buffer */
    static size_t buf_len = 0;        /* Amount of data in read_buffer */

    char chunk[128];                  /* Temporary buffer for reading chunks */
    time_t start_time, current_time;
    int remaining_timeout;
    ssize_t rc;
    size_t i;

    if (port == NULL || buffer == NULL || size <= 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || port->fd < 0) {
        return ERROR_IO;
    }

    start_time = time(NULL);
    buffer[0] = '\0';

    while (1) {
        /* First, check if we have a complete line in the buffer */
        for (i = buf_pos; i < buf_len; i++) {
            char c = read_buffer[i];

            /* Check for line terminators */
            if (c == '\n' || c == '\r') {
                /* Found line terminator - copy line to output buffer */
                size_t copy_len = i - buf_pos;
                if (copy_len > size - 1) {
                    copy_len = size - 1;
                }

                if (copy_len > 0) {
                    memcpy(buffer, &read_buffer[buf_pos], copy_len);
                }
                buffer[copy_len] = '\0';

                /* Skip the line terminator(s) */
                buf_pos = i + 1;

                /* Skip additional CR/LF if present */
                while (buf_pos < buf_len &&
                       (read_buffer[buf_pos] == '\r' || read_buffer[buf_pos] == '\n')) {
                    buf_pos++;
                }

                /* If we consumed all buffered data, reset buffer */
                if (buf_pos >= buf_len) {
                    buf_pos = 0;
                    buf_len = 0;
                }

                MB_LOG_DEBUG("serial_read_line: read line (%zu bytes): %s", copy_len, buffer);
                return (ssize_t)copy_len;
            }
        }

        /* No complete line found - need to read more data */
        current_time = time(NULL);
        remaining_timeout = timeout_sec - (current_time - start_time);

        if (remaining_timeout <= 0) {
            /* Timeout - return any partial data we have */
            if (buf_len > buf_pos) {
                size_t copy_len = buf_len - buf_pos;
                if (copy_len > size - 1) {
                    copy_len = size - 1;
                }
                memcpy(buffer, &read_buffer[buf_pos], copy_len);
                buffer[copy_len] = '\0';
                buf_pos = 0;
                buf_len = 0;
                MB_LOG_DEBUG("serial_read_line: timeout with partial data (%zu bytes)", copy_len);
                return ERROR_TIMEOUT;
            }
            MB_LOG_DEBUG("serial_read_line: timeout");
            return ERROR_TIMEOUT;
        }

        /* Compact buffer if needed (move remaining data to beginning) */
        if (buf_pos > 0) {
            if (buf_len > buf_pos) {
                memmove(read_buffer, &read_buffer[buf_pos], buf_len - buf_pos);
                buf_len -= buf_pos;
            } else {
                buf_len = 0;
            }
            buf_pos = 0;
        }

        /* Check if buffer is full without finding line terminator */
        if (buf_len >= sizeof(read_buffer) - 1) {
            /* Buffer overflow - return what we have and reset */
            size_t copy_len = (buf_len > size - 1) ? size - 1 : buf_len;
            memcpy(buffer, read_buffer, copy_len);
            buffer[copy_len] = '\0';
            buf_pos = 0;
            buf_len = 0;
            MB_LOG_WARNING("serial_read_line: buffer overflow, returning %zu bytes", copy_len);
            return (ssize_t)copy_len;
        }

        /* Read more data using select() for timeout */
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(port->fd, &readfds);

        tv.tv_sec = (remaining_timeout > 1) ? 1 : remaining_timeout;
        tv.tv_usec = 0;

        int sel = select(port->fd + 1, &readfds, NULL, NULL, &tv);

        if (sel < 0) {
            MB_LOG_ERROR("select() failed: %s", strerror(errno));
            return ERROR_IO;
        } else if (sel == 0) {
            /* Timeout on this select - continue loop to check overall timeout */
            continue;
        }

        /* Data available - read chunk */
        rc = read(port->fd, chunk, sizeof(chunk) - 1);

        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Shouldn't happen since select said readable, but handle anyway */
                continue;
            }
            MB_LOG_ERROR("read() failed: %s", strerror(errno));
            return ERROR_IO;
        } else if (rc > 0) {
            /* Got some data - append to buffer */
            size_t space_left = sizeof(read_buffer) - buf_len;
            size_t copy_len = ((size_t)rc > space_left) ? space_left : (size_t)rc;

            if (copy_len > 0) {
                memcpy(&read_buffer[buf_len], chunk, copy_len);
                buf_len += copy_len;
            }

            /* Continue loop to check if we now have a complete line */
        }
    }

    /* Should never reach here */
    return ERROR_IO;
}

/**
 * Lock serial port using UUCP-style lock file
 * Based on modem_sample/serial_port.c:lock_port()
 */
int serial_lock_port(const char *device)
{
    FILE *fp;
    pid_t pid;
    const char *devname;

    if (device == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Extract device name (ttyUSB0 from /dev/ttyUSB0) */
    devname = strrchr(device, '/');
    if (devname) {
        devname++;
    } else {
        devname = device;
    }

    /* Create lock file path */
    snprintf(g_lock_file, sizeof(g_lock_file), "/var/lock/LCK..%s", devname);

    /* Check if lock file exists */
    if (access(g_lock_file, F_OK) == 0) {
        /* Read PID from lock file */
        fp = fopen(g_lock_file, "r");
        if (fp) {
            if (fscanf(fp, "%d", &pid) == 1) {
                /* Check if process is still running */
                if (kill(pid, 0) == 0) {
                    fclose(fp);
                    MB_LOG_ERROR("Port locked by process %d", pid);
                    return ERROR_IO;
                } else {
                    /* Stale lock - remove it */
                    MB_LOG_INFO("Removing stale lock file (PID %d not running)", pid);
                    unlink(g_lock_file);
                }
            }
            fclose(fp);
        }
    }

    /* Create lock file */
    fp = fopen(g_lock_file, "w");
    if (!fp) {
        /* If we can't create lock file (no permissions), just warn and continue */
        MB_LOG_WARNING("Cannot create lock file %s: %s", g_lock_file, strerror(errno));
        MB_LOG_WARNING("Continuing without port locking...");
        g_lock_file[0] = '\0';
        return SUCCESS;
    }

    fprintf(fp, "%10d\n", getpid());
    fclose(fp);

    MB_LOG_INFO("Port locked: %s", g_lock_file);
    return SUCCESS;
}

/**
 * Unlock serial port
 * Based on modem_sample/serial_port.c:unlock_port()
 */
void serial_unlock_port(void)
{
    if (g_lock_file[0] != '\0') {
        if (unlink(g_lock_file) == 0) {
            MB_LOG_INFO("Port unlocked: %s", g_lock_file);
        }
        g_lock_file[0] = '\0';
    }
}

/**
 * Enable carrier detect (DCD) monitoring
 * Based on modem_sample/serial_port.c:enable_carrier_detect()
 */
int serial_enable_carrier_detect(serial_port_t *port)
{
    struct termios tios;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    MB_LOG_INFO("Enabling carrier detect (DCD monitoring)...");

    /* Get current settings */
    if (tcgetattr(port->fd, &tios) < 0) {
        MB_LOG_ERROR("tcgetattr failed: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Disable CLOCAL - enable carrier detect */
    tios.c_cflag &= ~CLOCAL;

    /* Enable RTS/CTS hardware flow control */
    tios.c_cflag |= CRTSCTS;

    /* Apply settings */
    if (tcsetattr(port->fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("tcsetattr failed: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_INFO("Carrier detect enabled - DCD signal will be monitored");
    return SUCCESS;
}

/**
 * Disable carrier detect (DCD) monitoring
 */
int serial_disable_carrier_detect(serial_port_t *port)
{
    struct termios tios;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    MB_LOG_INFO("Disabling carrier detect (ignoring DCD)...");

    /* Get current settings */
    if (tcgetattr(port->fd, &tios) < 0) {
        MB_LOG_ERROR("tcgetattr failed: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Enable CLOCAL - ignore carrier */
    tios.c_cflag |= CLOCAL;

    /* Apply settings */
    if (tcsetattr(port->fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("tcsetattr failed: %s", strerror(errno));
        return ERROR_IO;
    }

    MB_LOG_INFO("Carrier detect disabled - ignoring DCD signal");
    return SUCCESS;
}

/**
 * Check carrier (DCD) status
 * Based on modem_sample/serial_port.c:check_carrier_status()
 */
int serial_check_carrier(serial_port_t *port, bool *carrier)
{
    int dcd_state;

    if (port == NULL || carrier == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    dcd_state = serial_get_dcd(port);
    if (dcd_state < 0) {
        return ERROR_IO;
    }

    *carrier = (dcd_state > 0);
    return SUCCESS;
}

/**
 * Write data with robust error handling and retry logic
 * Based on modem_sample/serial_port.c:robust_serial_write()
 */
ssize_t serial_write_robust(serial_port_t *port, const void *buffer, size_t size)
{
    #define MAX_WRITE_RETRY 3
    #define RETRY_DELAY_US 100000  /* 100ms */

    ssize_t sent = 0;
    int retry = 0;
    ssize_t rc;
    bool carrier;

    if (port == NULL || buffer == NULL || size == 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || port->fd < 0) {
        return ERROR_IO;
    }

    /* Check carrier before starting transmission */
    if (serial_check_carrier(port, &carrier) == SUCCESS) {
        if (!carrier) {
            MB_LOG_ERROR("Carrier lost - cannot transmit");
            return ERROR_HANGUP;
        }
    }

    while (sent < (ssize_t)size && retry < MAX_WRITE_RETRY) {
        rc = write(port->fd, (const char *)buffer + sent, size - sent);

        if (rc < 0) {
            /* Check for hangup conditions */
            if (errno == EPIPE || errno == ECONNRESET) {
                MB_LOG_ERROR("Connection hangup during write (errno=%d)", errno);
                return ERROR_HANGUP;
            }

            /* Retry on temporary errors */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                MB_LOG_DEBUG("Write would block, retry %d/%d", retry + 1, MAX_WRITE_RETRY);
                usleep(RETRY_DELAY_US);
                retry++;
                continue;
            }

            /* Other errors are fatal */
            MB_LOG_ERROR("Write error: %s", strerror(errno));
            return ERROR_IO;
        }

        /* Successful write */
        sent += rc;
        retry = 0;  /* Reset retry counter on success */

        /* Check for partial write */
        if (rc < (ssize_t)(size - sent)) {
            MB_LOG_DEBUG("Partial write: sent %zd of %zu bytes, continuing...", sent, size);
        }
    }

    if (sent < (ssize_t)size) {
        MB_LOG_ERROR("Failed to send all data after %d retries: sent %zd of %zu bytes",
                    MAX_WRITE_RETRY, sent, size);
        return ERROR_IO;
    }

    /* Wait for data to be transmitted */
    tcdrain(port->fd);

    MB_LOG_DEBUG("Robust write: sent %zd bytes successfully", sent);
    return sent;

    #undef MAX_WRITE_RETRY
    #undef RETRY_DELAY_US
}

/**
 * Perform DTR drop hangup
 * Based on modem_sample/serial_port.c:dtr_drop_hangup()
 */
int serial_dtr_drop_hangup(serial_port_t *port)
{
    struct termios tios;
    speed_t saved_ispeed, saved_ospeed;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    MB_LOG_INFO("Performing DTR drop hangup...");

    /* Get current settings */
    if (tcgetattr(port->fd, &tios) < 0) {
        MB_LOG_WARNING("tcgetattr failed: %s (continuing anyway)", strerror(errno));
        return SUCCESS;  /* Not fatal - carrier may already be lost */
    }

    /* Save current speeds */
    saved_ispeed = cfgetispeed(&tios);
    saved_ospeed = cfgetospeed(&tios);

    /* Set speed to 0 to drop DTR */
    cfsetispeed(&tios, B0);
    cfsetospeed(&tios, B0);

    if (tcsetattr(port->fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_WARNING("tcsetattr (DTR drop) failed: %s (continuing anyway)", strerror(errno));
        return SUCCESS;  /* Not fatal */
    }

    MB_LOG_INFO("DTR dropped - waiting 1 second...");

    /* Wait for modem to recognize hangup */
    sleep(1);

    /* Restore original speeds (may fail if carrier is lost) */
    cfsetispeed(&tios, saved_ispeed);
    cfsetospeed(&tios, saved_ospeed);

    if (tcsetattr(port->fd, TCSADRAIN, &tios) < 0) {
        /* This is expected if the carrier is already lost */
        MB_LOG_INFO("DTR restore skipped (carrier already dropped)");
        return SUCCESS;  /* Not an error - connection is already closed */
    }

    MB_LOG_INFO("DTR drop hangup completed");
    return SUCCESS;
}

/**
 * Buffered serial transmission for large data
 * Based on modem_sample/serial_port.c:buffered_serial_send()
 */
ssize_t serial_write_buffered(serial_port_t *port, const void *buffer, size_t size)
{
    const unsigned char *data = (const unsigned char *)buffer;
    ssize_t total_sent = 0;
    size_t remaining = size;
    size_t chunk_size;
    ssize_t rc;
    bool carrier;

    if (port == NULL || buffer == NULL || size == 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || port->fd < 0) {
        return ERROR_IO;
    }

    MB_LOG_INFO("Buffered transmission: %zu bytes in chunks of %d bytes",
                size, TX_CHUNK_SIZE);

    /* Check initial carrier state */
    if (serial_check_carrier(port, &carrier) == SUCCESS) {
        if (!carrier) {
            MB_LOG_ERROR("No carrier - cannot start buffered transmission");
            return ERROR_HANGUP;
        }
    }

    /* Transmit data in chunks */
    while (remaining > 0) {
        /* Calculate chunk size */
        chunk_size = (remaining > TX_CHUNK_SIZE) ? TX_CHUNK_SIZE : remaining;

        /* Send chunk using robust write */
        rc = serial_write_robust(port, data + total_sent, chunk_size);

        if (rc < 0) {
            MB_LOG_ERROR("Chunk transmission failed at offset %zd: %s",
                        total_sent, error_to_string(rc));

            /* Return partial success if some data was sent */
            if (total_sent > 0) {
                MB_LOG_WARNING("Partial transmission: %zd of %zu bytes sent",
                              total_sent, size);
                return total_sent;
            }

            return rc;  /* Return error code */
        }

        total_sent += rc;
        remaining -= rc;

        /* Progress reporting for large transfers */
        if (size > TX_CHUNK_SIZE * 4) {  /* Report for transfers > 1KB */
            MB_LOG_DEBUG("Transmission progress: %zd/%zu bytes (%.1f%%)",
                        total_sent, size, (100.0 * total_sent) / size);
        }

        /* Delay between chunks to prevent buffer overflow (modem_sample pattern) */
        if (remaining > 0) {
            usleep(TX_CHUNK_DELAY_US);  /* 10ms delay */
        }

        /* Check carrier periodically */
        if (total_sent % (TX_CHUNK_SIZE * 4) == 0) {
            if (serial_check_carrier(port, &carrier) == SUCCESS) {
                if (!carrier) {
                    MB_LOG_ERROR("Carrier lost during transmission at %zd bytes",
                                total_sent);

                    if (total_sent > 0) {
                        return total_sent;  /* Return partial success */
                    }

                    return ERROR_HANGUP;
                }
            }
        }
    }

    MB_LOG_INFO("Buffered transmission completed: %zd bytes sent successfully",
                total_sent);

    /* Final tcdrain to ensure all data is transmitted */
    tcdrain(port->fd);

    return total_sent;
}

/* ========================================================================
 * Level 3 Speed Control and Flow Management Functions
 * ======================================================================== */

/**
 * Initialize Level 3 serial configuration with sensible defaults
 */
int serial_init_level3_config(serial_port_t *port)
{
    if (port == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Set default Level 3 configuration */
    port->l3_config.fixed_dte_speed = B57600;        /* Default to 57600 */
    port->l3_config.use_fixed_speed = false;        /* Dynamic speed by default */
    port->l3_config.hardware_flow_control = false;  /* HW flow control disabled */
    port->l3_config.software_flow_control = false;  /* SW flow control disabled */
    port->l3_config.xon_char = 0x11;                /* DC1 XON */
    port->l3_config.xoff_char = 0x13;               /* DC3 XOFF */
    port->l3_config.low_speed_optimization = false; /* Disabled by default */
    port->l3_config.tx_buffer_size = 4096;          /* 4KB TX buffer */
    port->l3_config.rx_buffer_size = 4096;          /* 4KB RX buffer */

    /* Initialize flow control state */
    port->tx_blocked = false;
    port->rx_blocked = false;
    port->last_xoff_time = 0;
    port->tx_flow_watermark = 3072;  /* 75% of TX buffer */
    port->rx_flow_watermark = 3072;  /* 75% of RX buffer */

    MB_LOG_DEBUG("Level 3 serial configuration initialized with defaults");
    return SUCCESS;
}

/**
 * Configure fixed DTE speed for Level 3 operations
 */
int serial_set_fixed_dte_speed(serial_port_t *port, speed_t fixed_speed, bool enable)
{
    if (port == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Validate fixed speed */
    switch (fixed_speed) {
        case B2400:
        case B4800:
        case B9600:
        case B19200:
        case B38400:
        case B57600:
        case B115200:
        case B230400:
            /* Valid speeds */
            break;
        default:
            MB_LOG_ERROR("Invalid fixed DTE speed: %d", fixed_speed);
            return ERROR_INVALID_ARG;
    }

    port->l3_config.fixed_dte_speed = fixed_speed;
    port->l3_config.use_fixed_speed = enable;

    /* If port is open, apply the configuration immediately */
    if (port->is_open) {
        int ret = serial_apply_level3_config(port);
        if (ret != SUCCESS) {
            return ret;
        }
    }

    MB_LOG_INFO("Fixed DTE speed %s: %d baud",
                enable ? "enabled" : "disabled", fixed_speed);
    return SUCCESS;
}

/**
 * Configure hardware flow control (RTS/CTS)
 */
int serial_set_hardware_flow_control(serial_port_t *port, bool enable)
{
    if (port == NULL) {
        return ERROR_INVALID_ARG;
    }

    port->l3_config.hardware_flow_control = enable;

    /* If port is open, apply the configuration immediately */
    if (port->is_open) {
        int ret = serial_apply_level3_config(port);
        if (ret != SUCCESS) {
            return ret;
        }
    }

    MB_LOG_INFO("Hardware flow control (RTS/CTS) %s", enable ? "enabled" : "disabled");
    return SUCCESS;
}

/**
 * Configure software flow control (XON/XOFF)
 */
int serial_set_software_flow_control(serial_port_t *port, bool enable,
                                   int xon_char, int xoff_char)
{
    if (port == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Validate XON/XOFF characters */
    if (xon_char < 0 || xon_char > 255 || xoff_char < 0 || xoff_char > 255) {
        MB_LOG_ERROR("Invalid XON/XOFF characters: XON=0x%02X, XOFF=0x%02X",
                     xon_char, xoff_char);
        return ERROR_INVALID_ARG;
    }

    port->l3_config.software_flow_control = enable;
    port->l3_config.xon_char = xon_char;
    port->l3_config.xoff_char = xoff_char;

    /* If port is open, apply the configuration immediately */
    if (port->is_open) {
        int ret = serial_apply_level3_config(port);
        if (ret != SUCCESS) {
            return ret;
        }
    }

    MB_LOG_INFO("Software flow control (XON/XOFF) %s: XON=0x%02X, XOFF=0x%02X",
                enable ? "enabled" : "disabled", xon_char, xoff_char);
    return SUCCESS;
}

/**
 * Enable low-speed optimizations for 1200 bps connections
 */
int serial_enable_low_speed_optimization(serial_port_t *port, bool enable)
{
    if (port == NULL) {
        return ERROR_INVALID_ARG;
    }

    port->l3_config.low_speed_optimization = enable;

    /* Adjust buffer sizes for low-speed operation */
    if (enable) {
        port->l3_config.tx_buffer_size = 1024;  /* Smaller buffers for low speed */
        port->l3_config.rx_buffer_size = 1024;
        port->tx_flow_watermark = 768;          /* 75% of 1KB */
        port->rx_flow_watermark = 768;
        MB_LOG_INFO("Low-speed optimizations enabled (1200 bps mode)");
    } else {
        port->l3_config.tx_buffer_size = 4096;  /* Normal buffers */
        port->l3_config.rx_buffer_size = 4096;
        port->tx_flow_watermark = 3072;         /* 75% of 4KB */
        port->rx_flow_watermark = 3072;
        MB_LOG_INFO("Low-speed optimizations disabled (normal speed mode)");
    }

    return SUCCESS;
}

/**
 * Apply Level 3 configuration to serial port
 */
int serial_apply_level3_config(serial_port_t *port)
{
    struct termios tios;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        return ERROR_IO;
    }

    /* Get current settings */
    if (tcgetattr(port->fd, &tios) < 0) {
        MB_LOG_ERROR("Failed to get terminal attributes for Level 3 config: %s",
                     strerror(errno));
        return ERROR_IO;
    }

    /* Apply fixed speed if enabled */
    if (port->l3_config.use_fixed_speed) {
        cfsetispeed(&tios, port->l3_config.fixed_dte_speed);
        cfsetospeed(&tios, port->l3_config.fixed_dte_speed);
        MB_LOG_DEBUG("Applied fixed DTE speed: %d baud", port->l3_config.fixed_dte_speed);
    }

    /* Configure flow control */
    /* Clear existing flow control flags first */
    tios.c_iflag &= ~(IXON | IXOFF | IXANY);
    tios.c_cflag &= ~CRTSCTS;

    if (port->l3_config.hardware_flow_control) {
        tios.c_cflag |= CRTSCTS;
        MB_LOG_DEBUG("Hardware flow control (RTS/CTS) enabled");
    }

    if (port->l3_config.software_flow_control) {
        tios.c_iflag |= IXON | IXOFF;
        /* Set XON/XOFF characters */
        tios.c_cc[VSTART] = port->l3_config.xon_char;
        tios.c_cc[VSTOP] = port->l3_config.xoff_char;
        MB_LOG_DEBUG("Software flow control enabled: XON=0x%02X, XOFF=0x%02X",
                     port->l3_config.xon_char, port->l3_config.xoff_char);
    }

    /* Apply settings */
    if (tcsetattr(port->fd, TCSADRAIN, &tios) < 0) {
        MB_LOG_ERROR("Failed to apply Level 3 configuration: %s", strerror(errno));
        return ERROR_IO;
    }

    /* Update stored baudrate if we changed it */
    if (port->l3_config.use_fixed_speed) {
        port->baudrate = port->l3_config.fixed_dte_speed;
    }

    MB_LOG_DEBUG("Level 3 configuration applied successfully");
    return SUCCESS;
}

/**
 * Handle incoming flow control characters (XON/XOFF)
 */
int serial_handle_flow_control(serial_port_t *port, const char *data, size_t len)
{
    size_t i;

    if (port == NULL || data == NULL || len == 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->l3_config.software_flow_control) {
        return SUCCESS;  /* Software flow control not enabled */
    }

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        if (c == port->l3_config.xon_char) {
            /* XON received - resume transmission */
            port->tx_blocked = false;
            MB_LOG_DEBUG("XON received - TX resumed");
        } else if (c == port->l3_config.xoff_char) {
            /* XOFF received - pause transmission */
            port->tx_blocked = true;
            port->last_xoff_time = time(NULL);
            MB_LOG_DEBUG("XOFF received - TX paused");
        }
    }

    return SUCCESS;
}

/**
 * Check if TX is blocked by flow control
 */
bool serial_is_tx_blocked(serial_port_t *port)
{
    if (port == NULL) {
        return false;
    }

    return port->tx_blocked;
}

/**
 * Check if RX is blocked by flow control
 */
bool serial_is_rx_blocked(serial_port_t *port)
{
    if (port == NULL) {
        return false;
    }

    return port->rx_blocked;
}

/**
 * Send XON character to resume flow
 */
int serial_send_xon(serial_port_t *port)
{
    char xon_char;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || !port->l3_config.software_flow_control) {
        return ERROR_IO;
    }

    xon_char = (char)port->l3_config.xon_char;

    ssize_t written = write(port->fd, &xon_char, 1);
    if (written != 1) {
        MB_LOG_ERROR("Failed to send XON: %s", strerror(errno));
        return ERROR_IO;
    }

    tcdrain(port->fd);  /* Ensure XON is sent */
    MB_LOG_DEBUG("XON sent to remote");

    return SUCCESS;
}

/**
 * Send XOFF character to pause flow
 */
int serial_send_xoff(serial_port_t *port)
{
    char xoff_char;

    if (port == NULL || port->fd < 0) {
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open || !port->l3_config.software_flow_control) {
        return ERROR_IO;
    }

    xoff_char = (char)port->l3_config.xoff_char;

    ssize_t written = write(port->fd, &xoff_char, 1);
    if (written != 1) {
        MB_LOG_ERROR("Failed to send XOFF: %s", strerror(errno));
        return ERROR_IO;
    }

    tcdrain(port->fd);  /* Ensure XOFF is sent */
    MB_LOG_DEBUG("XOFF sent to remote");

    return SUCCESS;
}

/**
 * Get optimal buffer size for current speed and flow control
 */
size_t serial_get_optimal_buffer_size(serial_port_t *port, bool is_tx)
{
    if (port == NULL) {
        return 1024;  /* Safe default */
    }

    size_t base_size = is_tx ? port->l3_config.tx_buffer_size : port->l3_config.rx_buffer_size;

    /* Adjust for low-speed optimizations */
    if (port->l3_config.low_speed_optimization) {
        return base_size;  /* Already adjusted for low speed */
    }

    /* Adjust based on current baudrate */
    if (port->baudrate <= B1200) {
        return 512;   /* Very small for 1200 bps */
    } else if (port->baudrate <= B2400) {
        return 1024;  /* Small for 2400 bps */
    } else if (port->baudrate <= B9600) {
        return 2048;  /* Medium for 9600 bps */
    } else {
        return base_size;  /* Full size for higher speeds */
    }
}

/**
 * Calculate transmission delay for current baudrate
 */
useconds_t serial_calculate_tx_delay(serial_port_t *port, size_t bytes)
{
    if (port == NULL || port->baudrate == 0) {
        return 1000;  /* 1ms default */
    }

    /* Calculate bits to transmit (8 data bits + 1 start bit + 1 stop bit = 10 bits per byte) */
    double bits_per_byte = 10.0;
    double total_bits = bytes * bits_per_byte;
    double baudrate = (double)port->baudrate;
    
    /* Calculate transmission time in microseconds */
    double delay_us = (total_bits / baudrate) * 1000000.0;
    
    /* Add safety margin (20% extra) */
    delay_us *= 1.2;
    
    /* Add minimum delay for system overhead */
    if (delay_us < 500) {
        delay_us = 500;  /* Minimum 0.5ms */
    }
    
    /* Cap maximum delay */
    if (delay_us > 5000000) {
        delay_us = 5000000;  /* Maximum 5 seconds */
    }
    
    return (useconds_t)delay_us;
}

/* ========================================================================
 * Level 1 Dynamic Baudrate Adjustment Functions (modem_sample integration)
 * ======================================================================== */

/**
 * Dynamically adjust serial port speed based on modem connection
 * Based on modem_sample/serial_port.c:adjust_serial_speed()
 */
int serial_adjust_baudrate_dynamically(serial_port_t *port, int target_speed)
{
    speed_t new_speed;
    int rc;

    if (port == NULL || port->fd < 0) {
        MB_LOG_ERROR("Invalid serial port for dynamic adjustment");
        return ERROR_INVALID_ARG;
    }

    if (!port->is_open) {
        MB_LOG_ERROR("Serial port not open for dynamic adjustment");
        return ERROR_IO;
    }

    if (target_speed <= 0) {
        MB_LOG_ERROR("Invalid target speed: %d", target_speed);
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("=== DYNAMIC BAUDRATE ADJUSTMENT ===");
    MB_LOG_INFO("Current baudrate: %d", port->baudrate);
    MB_LOG_INFO("Target baudrate: %d", target_speed);

    /* Convert target speed to speed_t */
    new_speed = serial_baudrate_to_speed_t(target_speed);
    if (new_speed == B0) {
        MB_LOG_ERROR("Unsupported target speed: %d", target_speed);
        return ERROR_INVALID_ARG;
    }

    /* Check if adjustment is needed */
    if (new_speed == port->baudrate) {
        MB_LOG_INFO("Speed already matches - no adjustment needed");
        return SUCCESS;
    }

    MB_LOG_INFO("Adjusting serial port speed: %d -> %d baud", port->baudrate, target_speed);

    /* Flush input/output buffers before speed change */
    tcflush(port->fd, TCIOFLUSH);

    /* Apply new baudrate */
    rc = serial_set_baudrate(port, new_speed);
    if (rc != SUCCESS) {
        MB_LOG_ERROR("Failed to set new baudrate: %s", error_to_string(rc));
        return rc;
    }

    /* Wait for hardware to stabilize at new speed */
    usleep(100000);  /* 100ms */

    /* Verify new speed setting */
    struct termios tios;
    if (tcgetattr(port->fd, &tios) == 0) {
        speed_t actual_ispeed = cfgetispeed(&tios);
        speed_t actual_ospeed = cfgetospeed(&tios);

        if (actual_ispeed == new_speed && actual_ospeed == new_speed) {
            MB_LOG_INFO("Speed adjustment verified: %d baud", target_speed);
        } else {
            MB_LOG_WARNING("Speed verification failed: ispeed=%d, ospeed=%d, expected=%d",
                          actual_ispeed, actual_ospeed, new_speed);
            /* Not a fatal error - continue */
        }
    }

    /* Additional delay for complete hardware stabilization */
    usleep(50000);  /* 50ms */

    MB_LOG_INFO("=== DYNAMIC BAUDRATE ADJUSTMENT COMPLETED ===");
    MB_LOG_INFO("Successfully adjusted to %d baud", target_speed);

    return SUCCESS;
}

/**
 * Validate serial port speed is within supported range
 */
bool serial_is_valid_speed(int speed)
{
    switch (speed) {
        case 300:
        case 1200:
        case 2400:
        case 4800:
        case 9600:
        case 19200:
        case 38400:
        case 57600:
        case 115200:
        case 230400:
            return true;
        default:
            return false;
    }
}

/**
 * Convert baudrate integer to speed_t (helper function)
 * Already exists as serial_baudrate_to_speed_t() but keeping this for completeness
 */
speed_t serial_baudrate_to_speed_t(int baudrate)
{
    switch (baudrate) {
        case 300:    return B300;
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:
            MB_LOG_WARNING("Unsupported baudrate %d, using 9600", baudrate);
            return B9600;
    }
}

/**
 * Check if data is available in serial input buffer
 * Based on modem_sample pattern for non-blocking availability check
 */
bool serial_check_available(serial_port_t *port)
{
    fd_set readfds;
    struct timeval tv;
    int rc;

    if (port == NULL || port->fd < 0) {
        return false;
    }

    if (!port->is_open) {
        return false;
    }

    FD_ZERO(&readfds);
    FD_SET(port->fd, &readfds);

    /* Zero timeout - just check availability */
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    rc = select(port->fd + 1, &readfds, NULL, NULL, &tv);

    return (rc > 0 && FD_ISSET(port->fd, &readfds));
}
