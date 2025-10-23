/*
 * level3_util.h - Utility functions for Level 3 Pipeline Management
 *
 * This file contains declarations for independent utility functions
 * extracted from level3.c for better modularity.
 */

#ifndef MODEMBRIDGE_LEVEL3_UTIL_H
#define MODEMBRIDGE_LEVEL3_UTIL_H

#include <stddef.h>
#include "level3_types.h"  /* Include type definitions */

/* Time utilities */

/**
 * Get current timestamp in milliseconds
 * @return Current timestamp in milliseconds
 */
long long l3_get_timestamp_ms(void);

/**
 * Get monotonic time in milliseconds (for duration measurements)
 * @return Monotonic time in milliseconds
 */
long long l3_get_monotonic_ms(void);

/* String conversion utilities */

/**
 * Convert pipeline state to string representation
 * @param state Pipeline state
 * @return String representation of the state
 */
const char *l3_pipeline_state_to_string(l3_pipeline_state_t state);

/**
 * Get direction name as string for logging
 * @param direction Direction enum
 * @return String representation of direction
 */
const char *l3_get_direction_name(l3_pipeline_direction_t direction);

/**
 * Get pipeline name from direction
 * @param direction Pipeline direction
 * @return Human-readable pipeline name
 */
const char *l3_get_pipeline_name(l3_pipeline_direction_t direction);

/**
 * Get state name as string
 * @param state System state
 * @return String representation of state
 */
const char *l3_get_state_name(l3_system_state_t state);

/**
 * Get watermark level name as string
 * @param level Watermark level
 * @return String representation of level
 */
const char *l3_watermark_level_to_string(l3_watermark_level_t level);

/* Formatting utilities */

/**
 * Format throughput value for display
 * @param bytes_per_second Throughput in bytes per second
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Formatted string
 */
const char *l3_format_throughput(double bytes_per_second, char *buffer, size_t buffer_size);

#endif /* MODEMBRIDGE_LEVEL3_UTIL_H */