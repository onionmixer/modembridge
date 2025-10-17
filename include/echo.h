/*
 * echo.h - Client echo functionality with timestamp formatting for ModemBridge
 *
 * Provides configurable echo functionality that intercepts client input in Level 1 mode
 * and echoes it back with timestamp formatting: "[timestamp][from server] DATA"
 */

#ifndef ECHO_H
#define ECHO_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "util.h"
#include "serial.h"

/* Echo result codes - now using common utility result types */
typedef enum {
    ECHO_SUCCESS = UTIL_RESULT_SUCCESS,
    ECHO_ERROR = UTIL_RESULT_FAILURE,
    ECHO_DISABLED = -5,
    ECHO_INVALID_PARAM = UTIL_RESULT_INVALID_PARAM,
    ECHO_BUFFER_FULL = UTIL_RESULT_BUFFER_FULL
} echo_result_t;

/* Echo control structure */
typedef struct {
    /* Common transmission control */
    util_transmission_ctrl_t transmission;

    /* Echo-specific timing control */
    time_t connect_time;       /* When client connected (for timing) */
    int first_delay;           /* Delay before first echo (seconds) */
    int min_interval;          /* Minimum interval between echoes (seconds) */
    time_t last_echo;          /* Timestamp of last echo sent */

    /* Internal state */
    bool online;               /* Whether client is online (connected) */
    char buffer[UTIL_MAX_MESSAGE_LEN];  /* Buffer for building echo messages */
    size_t buffer_len;         /* Current length of data in buffer */
} echo_ctrl_t;

/* Echo functionality initialization and control */
void echo_init(echo_ctrl_t *echo);
void echo_enable(echo_ctrl_t *echo, bool immediate, int first_delay, int min_interval);
void echo_disable(echo_ctrl_t *echo);
void echo_set_online(echo_ctrl_t *echo);
void echo_set_offline(echo_ctrl_t *echo);

/* Configuration functions */
void echo_set_prefix(echo_ctrl_t *echo, const char *prefix);
void echo_set_transmission(echo_ctrl_t *echo, int write_timeout_ms,
                          int max_retries, int retry_delay_ms);

/* Core echo functionality */
echo_result_t echo_process_client_data(echo_ctrl_t *echo, serial_port_t *port,
                                      const unsigned char *data, size_t len);
echo_result_t echo_send_formatted(echo_ctrl_t *echo, serial_port_t *port,
                                 const char *client_data, size_t len);
echo_result_t echo_flush_buffer(echo_ctrl_t *echo, serial_port_t *port);

/* Utility functions */
bool echo_should_send(echo_ctrl_t *echo);
int echo_format_message(echo_ctrl_t *echo, const char *client_data,
                       size_t client_len, char *buffer, size_t buffer_size);
void echo_reset_buffer(echo_ctrl_t *echo);
bool echo_is_buffer_full(echo_ctrl_t *echo);

/* Statistics and debugging */
void echo_get_stats(echo_ctrl_t *echo, int *total_echoes, int *total_failed);
void echo_print_status(echo_ctrl_t *echo);

#endif /* ECHO_H */