#ifndef MODEMBRIDGE_UTIL_H
#define MODEMBRIDGE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "serial.h"

/* Common result types for transmission operations */
typedef enum {
    UTIL_RESULT_SUCCESS = 0,
    UTIL_RESULT_FAILURE = -1,
    UTIL_RESULT_BUFFER_FULL = -2,
    UTIL_RESULT_TIMEOUT = -3,
    UTIL_RESULT_INVALID_PARAM = -4
} util_result_t;

/* Common transmission control structure */
typedef struct {
    bool enabled;
    bool immediate;
    int first_delay;
    int min_interval;
    char prefix[64];
    char suffix[64];
    int write_timeout_ms;
    int retry_count;
    int retry_delay_ms;
    bool online_mode;

    /* Timing and statistics */
    uint64_t last_sent_time;
    int total_sent;
    int total_failed;
    uint64_t next_due_time;
} util_transmission_ctrl_t;

/* Common buffer management structure */
typedef struct {
    unsigned char *buffer;
    size_t size;
    size_t head;
    size_t tail;
    size_t count;
    bool overflow_warned;
} util_circular_buffer_t;

/* Common statistics structure */
typedef struct {
    int total_operations;
    int successful_operations;
    int failed_operations;
    uint64_t total_bytes;
    uint64_t last_operation_time;
    double average_latency_ms;
} util_stats_t;

/* Initialize transmission control structure */
void util_transmission_init(util_transmission_ctrl_t *ctrl);

/* Enable/disable transmission */
void util_transmission_enable(util_transmission_ctrl_t *ctrl, bool immediate,
                              int first_delay, int min_interval);

void util_transmission_disable(util_transmission_ctrl_t *ctrl);

/* Set online/offline mode */
void util_transmission_set_online(util_transmission_ctrl_t *ctrl);
void util_transmission_set_offline(util_transmission_ctrl_t *ctrl);

/* Set transmission parameters */
void util_transmission_set_prefix(util_transmission_ctrl_t *ctrl, const char *prefix);
void util_transmission_set_suffix(util_transmission_ctrl_t *ctrl, const char *suffix);
void util_transmission_set_timing(util_transmission_ctrl_t *ctrl, int write_timeout_ms,
                                  int retry_count, int retry_delay_ms);

/* Check if transmission should occur */
bool util_transmission_should_send(util_transmission_ctrl_t *ctrl);

/* Format message with prefix/suffix */
int util_transmission_format_message(util_transmission_ctrl_t *ctrl,
                                     const char *content, char *buffer, size_t buffer_size);

/* Send formatted data */
util_result_t util_transmission_send(serial_port_t *port, util_transmission_ctrl_t *ctrl,
                                     const char *content);

/* Get next due time */
int util_transmission_get_next_due(util_transmission_ctrl_t *ctrl);

/* Get statistics */
void util_transmission_get_stats(util_transmission_ctrl_t *ctrl, int *total_sent,
                                 int *total_failed);

/* Print status */
void util_transmission_print_status(util_transmission_ctrl_t *ctrl, const char *name);

/* Circular buffer operations */
void util_cbuf_init(util_circular_buffer_t *buf, unsigned char *buffer, size_t size);
size_t util_cbuf_write(util_circular_buffer_t *buf, const unsigned char *data, size_t len);
size_t util_cbuf_read(util_circular_buffer_t *buf, unsigned char *data, size_t len);
size_t util_cbuf_available(util_circular_buffer_t *buf);
size_t util_cbuf_free(util_circular_buffer_t *buf);
bool util_cbuf_is_empty(util_circular_buffer_t *buf);
bool util_cbuf_is_full(util_circular_buffer_t *buf);
void util_cbuf_clear(util_circular_buffer_t *buf);

/* Statistics operations */
void util_stats_init(util_stats_t *stats);
void util_stats_update(util_stats_t *stats, bool success, size_t bytes, double latency_ms);
void util_stats_print(util_stats_t *stats, const char *operation_name);

/* Common constants */
#define UTIL_MAX_MESSAGE_LEN        512
#define UTIL_DEFAULT_PREFIX         "[modembridge]"
#define UTIL_DEFAULT_ENABLED        false
#define UTIL_DEFAULT_IMMEDIATE      true
#define UTIL_DEFAULT_WRITE_TIMEOUT  1000
#define UTIL_DEFAULT_RETRY_COUNT    3
#define UTIL_DEFAULT_RETRY_DELAY    100

#endif /* MODEMBRIDGE_UTIL_H */