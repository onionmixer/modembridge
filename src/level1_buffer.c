/*
 * level1_buffer.c - Buffer management functions for Level 1 (Serial/Modem)
 *
 * This file contains implementations for circular buffer management functions
 * used by Level 1 components. Includes both basic and thread-safe versions.
 */

#include "level1_buffer.h"
#include <string.h>
#include <errno.h>
#include <time.h>

/* ========== Basic Circular Buffer Functions ========== */

/**
 * Initialize circular buffer
 */
void cbuf_init(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return;
    }

    memset(buf, 0, sizeof(circular_buffer_t));
    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->count = 0;
}

/**
 * Write data to circular buffer
 */
size_t cbuf_write(circular_buffer_t *buf, const unsigned char *data, size_t len)
{
    size_t written = 0;

    if (buf == NULL || data == NULL) {
        return 0;
    }

    while (written < len && buf->count < BUFFER_SIZE) {
        buf->data[buf->write_pos] = data[written];
        buf->write_pos = (buf->write_pos + 1) % BUFFER_SIZE;
        buf->count++;
        written++;
    }

    return written;
}

/**
 * Read data from circular buffer
 */
size_t cbuf_read(circular_buffer_t *buf, unsigned char *data, size_t len)
{
    size_t read_count = 0;

    if (buf == NULL || data == NULL) {
        return 0;
    }

    while (read_count < len && buf->count > 0) {
        data[read_count] = buf->data[buf->read_pos];
        buf->read_pos = (buf->read_pos + 1) % BUFFER_SIZE;
        buf->count--;
        read_count++;
    }

    return read_count;
}

/**
 * Get available data in buffer
 */
size_t cbuf_available(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return 0;
    }

    return buf->count;
}

/**
 * Get free space in buffer
 */
size_t cbuf_free(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return 0;
    }

    return BUFFER_SIZE - buf->count;
}

/**
 * Check if buffer is empty
 */
bool cbuf_is_empty(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return true;
    }

    return buf->count == 0;
}

/**
 * Check if buffer is full
 */
bool cbuf_is_full(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return false;
    }

    return buf->count >= BUFFER_SIZE;
}

/**
 * Clear circular buffer
 */
void cbuf_clear(circular_buffer_t *buf)
{
    if (buf == NULL) {
        return;
    }

    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->count = 0;
}

/* ========== Thread-safe circular buffer functions ========== */

/**
 * Initialize thread-safe circular buffer
 */
void ts_cbuf_init(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return;
    }

    cbuf_init(&tsbuf->cbuf);
    pthread_mutex_init(&tsbuf->mutex, NULL);
    pthread_cond_init(&tsbuf->cond_not_empty, NULL);
    pthread_cond_init(&tsbuf->cond_not_full, NULL);
    tsbuf->initialized = true;
}

/**
 * Destroy thread-safe circular buffer
 */
void ts_cbuf_destroy(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return;
    }

    if (tsbuf->initialized) {
        pthread_mutex_destroy(&tsbuf->mutex);
        pthread_cond_destroy(&tsbuf->cond_not_empty);
        pthread_cond_destroy(&tsbuf->cond_not_full);
        tsbuf->initialized = false;
    }
}

/**
 * Write data to thread-safe circular buffer (non-blocking)
 */
size_t ts_cbuf_write(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Non-blocking: write what we can */
    size_t written = cbuf_write(&tsbuf->cbuf, data, len);

    /* Signal waiting readers if we wrote data */
    if (written > 0) {
        pthread_cond_signal(&tsbuf->cond_not_empty);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return written;
}

/**
 * Read data from thread-safe circular buffer (non-blocking)
 */
size_t ts_cbuf_read(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Non-blocking: read what's available */
    size_t read = cbuf_read(&tsbuf->cbuf, data, len);

    /* Signal waiting writers if we freed space */
    if (read > 0) {
        pthread_cond_signal(&tsbuf->cond_not_full);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return read;
}

/**
 * Write with timeout (blocking until space available or timeout)
 */
size_t ts_cbuf_write_timeout(ts_circular_buffer_t *tsbuf, const unsigned char *data,
                              size_t len, int timeout_ms)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Wait for space if buffer is full */
    while (cbuf_free(&tsbuf->cbuf) == 0) {
        int ret = pthread_cond_timedwait(&tsbuf->cond_not_full, &tsbuf->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&tsbuf->mutex);
            return 0; /* Timeout */
        }
    }

    size_t written = cbuf_write(&tsbuf->cbuf, data, len);

    if (written > 0) {
        pthread_cond_signal(&tsbuf->cond_not_empty);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return written;
}

/**
 * Read with timeout (blocking until data available or timeout)
 */
size_t ts_cbuf_read_timeout(ts_circular_buffer_t *tsbuf, unsigned char *data,
                             size_t len, int timeout_ms)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Wait for data if buffer is empty */
    while (cbuf_available(&tsbuf->cbuf) == 0) {
        int ret = pthread_cond_timedwait(&tsbuf->cond_not_empty, &tsbuf->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&tsbuf->mutex);
            return 0; /* Timeout */
        }
    }

    size_t read = cbuf_read(&tsbuf->cbuf, data, len);

    if (read > 0) {
        pthread_cond_signal(&tsbuf->cond_not_full);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return read;
}

/**
 * Check if buffer is empty (thread-safe)
 */
bool ts_cbuf_is_empty(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return true;
    }

    pthread_mutex_lock(&tsbuf->mutex);
    bool empty = cbuf_is_empty(&tsbuf->cbuf);
    pthread_mutex_unlock(&tsbuf->mutex);

    return empty;
}

/**
 * Get available data (thread-safe)
 */
size_t ts_cbuf_available(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return 0;
    }

    pthread_mutex_lock(&tsbuf->mutex);
    size_t available = cbuf_available(&tsbuf->cbuf);
    pthread_mutex_unlock(&tsbuf->mutex);

    return available;
}