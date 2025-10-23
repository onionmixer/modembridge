/*
 * level1_buffer.h - Buffer management functions for Level 1 (Serial/Modem)
 *
 * This file contains declarations for circular buffer management functions
 * used by Level 1 components. Includes both basic and thread-safe versions.
 */

#ifndef MODEMBRIDGE_LEVEL1_BUFFER_H
#define MODEMBRIDGE_LEVEL1_BUFFER_H

#include "level1_types.h"
#include <stddef.h>
#include <stdbool.h>

/* ========== Basic Circular Buffer Functions ========== */

/**
 * Initialize a circular buffer
 * @param buf Pointer to circular buffer structure
 */
void cbuf_init(circular_buffer_t *buf);

/**
 * Write data to circular buffer
 * @param buf Circular buffer
 * @param data Data to write
 * @param len Length of data
 * @return Number of bytes written
 */
size_t cbuf_write(circular_buffer_t *buf, const unsigned char *data, size_t len);

/**
 * Read data from circular buffer
 * @param buf Circular buffer
 * @param data Output buffer
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
size_t cbuf_read(circular_buffer_t *buf, unsigned char *data, size_t len);

/**
 * Get number of bytes available in buffer
 * @param buf Circular buffer
 * @return Number of bytes available for reading
 */
size_t cbuf_available(circular_buffer_t *buf);

/**
 * Get free space in buffer
 * @param buf Circular buffer
 * @return Number of bytes available for writing
 */
size_t cbuf_free(circular_buffer_t *buf);

/**
 * Check if buffer is empty
 * @param buf Circular buffer
 * @return true if empty, false otherwise
 */
bool cbuf_is_empty(circular_buffer_t *buf);

/**
 * Check if buffer is full
 * @param buf Circular buffer
 * @return true if full, false otherwise
 */
bool cbuf_is_full(circular_buffer_t *buf);

/**
 * Clear buffer contents
 * @param buf Circular buffer to clear
 */
void cbuf_clear(circular_buffer_t *buf);

/* ========== Thread-Safe Circular Buffer Functions ========== */

/**
 * Initialize a thread-safe circular buffer
 * @param tsbuf Pointer to thread-safe circular buffer
 */
void ts_cbuf_init(ts_circular_buffer_t *tsbuf);

/**
 * Destroy a thread-safe circular buffer (cleanup mutexes/conditions)
 * @param tsbuf Pointer to thread-safe circular buffer
 */
void ts_cbuf_destroy(ts_circular_buffer_t *tsbuf);

/**
 * Write data to thread-safe buffer (blocking)
 * @param tsbuf Thread-safe circular buffer
 * @param data Data to write
 * @param len Length of data
 * @return Number of bytes written
 */
size_t ts_cbuf_write(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len);

/**
 * Read data from thread-safe buffer (blocking)
 * @param tsbuf Thread-safe circular buffer
 * @param data Output buffer
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
size_t ts_cbuf_read(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len);

/**
 * Write data to thread-safe buffer with timeout
 * @param tsbuf Thread-safe circular buffer
 * @param data Data to write
 * @param len Length of data
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking, -1 = infinite)
 * @return Number of bytes written, or -1 on timeout
 */
size_t ts_cbuf_write_timeout(ts_circular_buffer_t *tsbuf, const unsigned char *data,
                             size_t len, int timeout_ms);

/**
 * Read data from thread-safe buffer with timeout
 * @param tsbuf Thread-safe circular buffer
 * @param data Output buffer
 * @param len Maximum bytes to read
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking, -1 = infinite)
 * @return Number of bytes read, or -1 on timeout
 */
size_t ts_cbuf_read_timeout(ts_circular_buffer_t *tsbuf, unsigned char *data,
                            size_t len, int timeout_ms);

/**
 * Check if thread-safe buffer is empty
 * @param tsbuf Thread-safe circular buffer
 * @return true if empty, false otherwise
 */
bool ts_cbuf_is_empty(ts_circular_buffer_t *tsbuf);

/**
 * Get number of bytes available in thread-safe buffer
 * @param tsbuf Thread-safe circular buffer
 * @return Number of bytes available for reading
 */
size_t ts_cbuf_available(ts_circular_buffer_t *tsbuf);

#endif /* MODEMBRIDGE_LEVEL1_BUFFER_H */