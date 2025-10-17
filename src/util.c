#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "util.h"
#include "common.h"

static uint64_t get_current_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void util_transmission_init(util_transmission_ctrl_t *ctrl)
{
    if (!ctrl) return;

    ctrl->enabled = UTIL_DEFAULT_ENABLED;
    ctrl->immediate = UTIL_DEFAULT_IMMEDIATE;
    ctrl->first_delay = 0;
    ctrl->min_interval = 1000;
    strcpy(ctrl->prefix, UTIL_DEFAULT_PREFIX);
    strcpy(ctrl->suffix, "");
    ctrl->write_timeout_ms = UTIL_DEFAULT_WRITE_TIMEOUT;
    ctrl->retry_count = UTIL_DEFAULT_RETRY_COUNT;
    ctrl->retry_delay_ms = UTIL_DEFAULT_RETRY_DELAY;
    ctrl->online_mode = false;
    ctrl->last_sent_time = 0;
    ctrl->total_sent = 0;
    ctrl->total_failed = 0;
    ctrl->next_due_time = 0;
}

void util_transmission_enable(util_transmission_ctrl_t *ctrl, bool immediate,
                              int first_delay, int min_interval)
{
    if (!ctrl) return;

    ctrl->enabled = true;
    ctrl->immediate = immediate;
    ctrl->first_delay = first_delay;
    ctrl->min_interval = min_interval;
    ctrl->last_sent_time = 0;
    ctrl->next_due_time = immediate ? 0 : get_current_time_ms() + first_delay;
}

void util_transmission_disable(util_transmission_ctrl_t *ctrl)
{
    if (!ctrl) return;
    ctrl->enabled = false;
}

void util_transmission_set_online(util_transmission_ctrl_t *ctrl)
{
    if (!ctrl) return;
    ctrl->online_mode = true;
}

void util_transmission_set_offline(util_transmission_ctrl_t *ctrl)
{
    if (!ctrl) return;
    ctrl->online_mode = false;
}

void util_transmission_set_prefix(util_transmission_ctrl_t *ctrl, const char *prefix)
{
    if (!ctrl || !prefix) return;
    strncpy(ctrl->prefix, prefix, sizeof(ctrl->prefix) - 1);
    ctrl->prefix[sizeof(ctrl->prefix) - 1] = '\0';
}

void util_transmission_set_suffix(util_transmission_ctrl_t *ctrl, const char *suffix)
{
    if (!ctrl || !suffix) return;
    strncpy(ctrl->suffix, suffix, sizeof(ctrl->suffix) - 1);
    ctrl->suffix[sizeof(ctrl->suffix) - 1] = '\0';
}

void util_transmission_set_timing(util_transmission_ctrl_t *ctrl, int write_timeout_ms,
                                  int retry_count, int retry_delay_ms)
{
    if (!ctrl) return;
    ctrl->write_timeout_ms = write_timeout_ms;
    ctrl->retry_count = retry_count;
    ctrl->retry_delay_ms = retry_delay_ms;
}

bool util_transmission_should_send(util_transmission_ctrl_t *ctrl)
{
    uint64_t current_time;

    if (!ctrl || !ctrl->enabled) {
        return false;
    }

    current_time = get_current_time_ms();

    /* Check if it's time to send */
    if (current_time < ctrl->next_due_time) {
        return false;
    }

    return true;
}

int util_transmission_format_message(util_transmission_ctrl_t *ctrl,
                                     const char *content, char *buffer, size_t buffer_size)
{
    int len = 0;

    if (!ctrl || !content || !buffer || buffer_size == 0) {
        return -1;
    }

    /* Add prefix */
    if (ctrl->prefix[0] != '\0') {
        len += snprintf(buffer + len, buffer_size - len, "%s ", ctrl->prefix);
        if (len >= (int)buffer_size) return -1;
    }

    /* Add content */
    len += snprintf(buffer + len, buffer_size - len, "%s", content);
    if (len >= (int)buffer_size) return -1;

    /* Add suffix */
    if (ctrl->suffix[0] != '\0') {
        len += snprintf(buffer + len, buffer_size - len, " %s", ctrl->suffix);
        if (len >= (int)buffer_size) return -1;
    }

    /* Add newline */
    len += snprintf(buffer + len, buffer_size - len, "\r\n");
    if (len >= (int)buffer_size) return -1;

    return len;
}

util_result_t util_transmission_send(serial_port_t *port, util_transmission_ctrl_t *ctrl,
                                     const char *content)
{
    char formatted_buffer[UTIL_MAX_MESSAGE_LEN];
    int formatted_len;
    ssize_t bytes_written;
    int retry_count = 0;
    uint64_t current_time;

    if (!port || !ctrl || !content || !ctrl->enabled) {
        return UTIL_RESULT_INVALID_PARAM;
    }

    current_time = get_current_time_ms();

    /* Check if it's time to send */
    if (current_time < ctrl->next_due_time) {
        return UTIL_RESULT_TIMEOUT;
    }

    /* Format the message */
    formatted_len = util_transmission_format_message(ctrl, content, formatted_buffer,
                                                     sizeof(formatted_buffer));
    if (formatted_len < 0) {
        return UTIL_RESULT_FAILURE;
    }

    /* Try to send with retries */
    do {
        bytes_written = serial_write(port, formatted_buffer, formatted_len);
        if (bytes_written == formatted_len) {
            ctrl->total_sent++;
            ctrl->last_sent_time = current_time;

            /* Schedule next transmission */
            ctrl->next_due_time = current_time + ctrl->min_interval;

            return UTIL_RESULT_SUCCESS;
        }

        /* Failed to write, wait and retry */
        if (retry_count < ctrl->retry_count) {
            usleep(ctrl->retry_delay_ms * 1000);
            retry_count++;
        }
    } while (retry_count <= ctrl->retry_count);

    ctrl->total_failed++;
    return UTIL_RESULT_FAILURE;
}

int util_transmission_get_next_due(util_transmission_ctrl_t *ctrl)
{
    if (!ctrl) return -1;

    uint64_t current_time = get_current_time_ms();
    if (current_time >= ctrl->next_due_time) {
        return 0;
    }

    return (int)(ctrl->next_due_time - current_time);
}

void util_transmission_get_stats(util_transmission_ctrl_t *ctrl, int *total_sent,
                                 int *total_failed)
{
    if (!ctrl) return;

    if (total_sent) *total_sent = ctrl->total_sent;
    if (total_failed) *total_failed = ctrl->total_failed;
}

void util_transmission_print_status(util_transmission_ctrl_t *ctrl, const char *name)
{
    if (!ctrl || !name) return;

    printf("%s Status:\n", name);
    printf("  Enabled: %s\n", ctrl->enabled ? "Yes" : "No");
    printf("  Online Mode: %s\n", ctrl->online_mode ? "Yes" : "No");
    printf("  Total Sent: %d\n", ctrl->total_sent);
    printf("  Total Failed: %d\n", ctrl->total_failed);
    printf("  Interval: %d ms\n", ctrl->min_interval);
    printf("  Prefix: \"%s\"\n", ctrl->prefix);
    printf("  Suffix: \"%s\"\n", ctrl->suffix);
}

/* Circular buffer operations */
void util_cbuf_init(util_circular_buffer_t *buf, unsigned char *buffer, size_t size)
{
    if (!buf) return;

    buf->buffer = buffer;
    buf->size = size;
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->overflow_warned = false;
}

size_t util_cbuf_write(util_circular_buffer_t *buf, const unsigned char *data, size_t len)
{
    size_t written = 0;
    size_t available;

    if (!buf || !data || len == 0) return 0;

    available = buf->size - buf->count;
    if (len > available) {
        len = available;
        if (!buf->overflow_warned) {
            MB_LOG_WARNING("Circular buffer overflow, data will be lost");
            buf->overflow_warned = true;
        }
    } else {
        buf->overflow_warned = false;
    }

    while (written < len && buf->count < buf->size) {
        buf->buffer[buf->head] = data[written];
        buf->head = (buf->head + 1) % buf->size;
        buf->count++;
        written++;
    }

    return written;
}

size_t util_cbuf_read(util_circular_buffer_t *buf, unsigned char *data, size_t len)
{
    size_t read = 0;

    if (!buf || !data || len == 0) return 0;

    while (read < len && buf->count > 0) {
        data[read] = buf->buffer[buf->tail];
        buf->tail = (buf->tail + 1) % buf->size;
        buf->count--;
        read++;
    }

    return read;
}

size_t util_cbuf_available(util_circular_buffer_t *buf)
{
    return buf ? buf->count : 0;
}

size_t util_cbuf_free(util_circular_buffer_t *buf)
{
    return buf ? buf->size - buf->count : 0;
}

bool util_cbuf_is_empty(util_circular_buffer_t *buf)
{
    return buf ? buf->count == 0 : true;
}

bool util_cbuf_is_full(util_circular_buffer_t *buf)
{
    return buf ? buf->count == buf->size : false;
}

void util_cbuf_clear(util_circular_buffer_t *buf)
{
    if (!buf) return;
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->overflow_warned = false;
}

/* Statistics operations */
void util_stats_init(util_stats_t *stats)
{
    if (!stats) return;

    stats->total_operations = 0;
    stats->successful_operations = 0;
    stats->failed_operations = 0;
    stats->total_bytes = 0;
    stats->last_operation_time = 0;
    stats->average_latency_ms = 0.0;
}

void util_stats_update(util_stats_t *stats, bool success, size_t bytes, double latency_ms)
{
    if (!stats) return;

    stats->total_operations++;
    stats->total_bytes += bytes;
    stats->last_operation_time = get_current_time_ms();

    if (success) {
        stats->successful_operations++;
    } else {
        stats->failed_operations++;
    }

    /* Update average latency */
    if (stats->total_operations > 0) {
        stats->average_latency_ms =
            (stats->average_latency_ms * (stats->total_operations - 1) + latency_ms) /
            stats->total_operations;
    }
}

void util_stats_print(util_stats_t *stats, const char *operation_name)
{
    if (!stats || !operation_name) return;

    printf("%s Statistics:\n", operation_name);
    printf("  Total Operations: %d\n", stats->total_operations);
    printf("  Successful: %d\n", stats->successful_operations);
    printf("  Failed: %d\n", stats->failed_operations);
    printf("  Success Rate: %.2f%%\n",
           stats->total_operations > 0 ?
           (double)stats->successful_operations / stats->total_operations * 100.0 : 0.0);
    printf("  Total Bytes: %lu\n", stats->total_bytes);
    printf("  Average Latency: %.2f ms\n", stats->average_latency_ms);
}