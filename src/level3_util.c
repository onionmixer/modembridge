/*
 * level3_util.c - Utility functions for Level 3 Pipeline Management
 *
 * This file contains independent utility functions extracted from level3.c
 * for better modularity and maintainability.
 */

#include "level3.h"
#include <sys/time.h>
#include <time.h>
#include <stdio.h>

/**
 * Get current timestamp in milliseconds
 * @return Current timestamp in milliseconds
 */
long long l3_get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * Convert pipeline state to string representation
 * @param state Pipeline state
 * @return String representation of the state
 */
const char *l3_pipeline_state_to_string(l3_pipeline_state_t state)
{
    switch (state) {
        case L3_PIPELINE_STATE_IDLE:
            return "IDLE";
        case L3_PIPELINE_STATE_ACTIVE:
            return "ACTIVE";
        case L3_PIPELINE_STATE_BLOCKED:
            return "BLOCKED";
        case L3_PIPELINE_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

/**
 * Get direction name as string for logging
 * @param direction Direction enum
 * @return String representation of direction
 */
const char *l3_get_direction_name(l3_pipeline_direction_t direction)
{
    switch (direction) {
        case L3_PIPELINE_SERIAL_TO_TELNET:
            return "Serial→Telnet";
        case L3_PIPELINE_TELNET_TO_SERIAL:
            return "Telnet→Serial";
        default:
            return "Unknown";
    }
}

/**
 * Get pipeline name from direction
 * @param direction Pipeline direction
 * @return Human-readable pipeline name
 */
const char *l3_get_pipeline_name(l3_pipeline_direction_t direction)
{
    return l3_get_direction_name(direction);  // Same implementation
}

/**
 * Get state name as string
 * @param state System state
 * @return String representation of state
 */
const char *l3_get_state_name(l3_system_state_t state)
{
    switch (state) {
        case L3_STATE_UNINITIALIZED:
            return "UNINITIALIZED";
        case L3_STATE_INITIALIZING:
            return "INITIALIZING";
        case L3_STATE_READY:
            return "READY";
        case L3_STATE_CONNECTING:
            return "CONNECTING";
        case L3_STATE_NEGOTIATING:
            return "NEGOTIATING";
        case L3_STATE_DATA_TRANSFER:
            return "DATA_TRANSFER";
        case L3_STATE_FLUSHING:
            return "FLUSHING";
        case L3_STATE_SHUTTING_DOWN:
            return "SHUTTING_DOWN";
        case L3_STATE_TERMINATED:
            return "TERMINATED";
        case L3_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

/**
 * Get watermark level name as string
 * @param level Watermark level
 * @return String representation of level
 */
const char *l3_watermark_level_to_string(l3_watermark_level_t level)
{
    switch (level) {
        case L3_WATERMARK_CRITICAL:
            return "CRITICAL";
        case L3_WATERMARK_HIGH:
            return "HIGH";
        case L3_WATERMARK_NORMAL:
            return "NORMAL";
        case L3_WATERMARK_LOW:
            return "LOW";
        case L3_WATERMARK_EMPTY:
            return "EMPTY";
        default:
            return "UNKNOWN";
    }
}

/**
 * Format throughput value for display
 * @param bytes_per_second Throughput in bytes per second
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Formatted string
 */
const char *l3_format_throughput(double bytes_per_second, char *buffer, size_t buffer_size)
{
    if (bytes_per_second < 1024.0) {
        snprintf(buffer, buffer_size, "%.2f B/s", bytes_per_second);
    } else if (bytes_per_second < 1024.0 * 1024.0) {
        snprintf(buffer, buffer_size, "%.2f KB/s", bytes_per_second / 1024.0);
    } else {
        snprintf(buffer, buffer_size, "%.2f MB/s", bytes_per_second / (1024.0 * 1024.0));
    }
    return buffer;
}

/**
 * Get monotonic time in milliseconds (for duration measurements)
 * @return Monotonic time in milliseconds
 */
long long l3_get_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}