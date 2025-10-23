/*
 * level3_schedule.c - Scheduling functions for Level 3 Pipeline Management
 *
 * This file contains scheduling and fairness functions extracted from level3.c
 * for better modularity and maintainability.
 */

#include "level3.h"           /* Must be included first for l3_context_t definition */
#include "level3_schedule.h"  /* Scheduling function declarations */
#include "level3_buffer.h"    /* Buffer management functions */
#include "level3_util.h"      /* Utility functions */
#include "common.h"
#include <string.h>
#include <pthread.h>
#include <time.h>

/* ========== Pipeline Scheduling Functions ========== */

/**
 * Initialize enhanced scheduling system with quantum enforcement
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_init_enhanced_scheduling(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        MB_LOG_ERROR("Invalid Level 3 context for scheduling initialization");
        return L3_ERROR_INVALID_PARAM;
    }

    MB_LOG_INFO("Initializing enhanced scheduling system with quantum enforcement");

    /* Initialize scheduling configuration */
    l3_ctx->sched_config.base_quantum_ms = 50;        /* 50ms base quantum */
    l3_ctx->sched_config.min_quantum_ms = 10;         /* 10ms minimum */
    l3_ctx->sched_config.max_quantum_ms = 200;        /* 200ms maximum */
    l3_ctx->sched_config.weight_balance_ratio = 0.6f; /* Favor serial slightly */
    l3_ctx->sched_config.starvation_threshold_ms = 500; /* 500ms starvation threshold */
    l3_ctx->sched_config.adaptive_quantum_enabled = true;
    l3_ctx->sched_config.fair_queue_enabled = true;

    /* Initialize latency tracking */
    l3_ctx->latency_stats.serial_to_telnet_avg_ms = 0.0;
    l3_ctx->latency_stats.telnet_to_serial_avg_ms = 0.0;
    l3_ctx->latency_stats.max_serial_to_telnet_ms = 0.0;
    l3_ctx->latency_stats.max_telnet_to_serial_ms = 0.0;
    l3_ctx->latency_stats.total_samples = 0;
    l3_ctx->latency_stats.last_measurement_time = 0;

    /* Initialize scheduling state */
    long long init_time = l3_get_timestamp_ms();
    l3_ctx->sched_state.current_direction = L3_PIPELINE_SERIAL_TO_TELNET;
    l3_ctx->sched_state.last_direction_switch_time = init_time;
    l3_ctx->sched_state.consecutive_slices = 0;
    l3_ctx->sched_state.serial_starvation_time = init_time;  /* Initialize to current time */
    l3_ctx->sched_state.telnet_starvation_time = init_time;  /* Initialize to current time */

    /* Initialize quantum state */
    l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.base_quantum_ms;
    l3_ctx->quantum_state.start_time = 0;
    l3_ctx->quantum_state.bytes_processed = 0;
    l3_ctx->quantum_state.max_bytes_per_quantum = 1024; /* Conservative start */

    /* Initialize fair queue weights */
    l3_ctx->fair_queue.serial_weight = 5;
    l3_ctx->fair_queue.telnet_weight = 5;
    l3_ctx->fair_queue.serial_deficit = 0;
    l3_ctx->fair_queue.telnet_deficit = 0;

    MB_LOG_INFO("Enhanced scheduling initialized - base quantum: %dms, balance ratio: %.2f",
                l3_ctx->sched_config.base_quantum_ms, l3_ctx->sched_config.weight_balance_ratio);

    return L3_SUCCESS;
}

l3_pipeline_direction_t l3_schedule_next_pipeline(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_PIPELINE_SERIAL_TO_TELNET;
    }

    pthread_mutex_lock(&l3_ctx->scheduling_mutex);

    /* Simple round-robin scheduling with fairness considerations */
    l3_pipeline_direction_t next_pipeline;

    /* Check if current pipeline has work to do */
    l3_pipeline_t *current = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                             &l3_ctx->pipeline_serial_to_telnet :
                             &l3_ctx->pipeline_telnet_to_serial;

    size_t current_available = l3_double_buffer_available(&current->buffers);

    /* If current pipeline has data and hasn't exceeded its timeslice, continue with it */
    if (current_available > 0 && current->bytes_in_timeslice < L3_MAX_BURST_SIZE) {
        next_pipeline = l3_ctx->active_pipeline;
    } else {
        /* Switch to the other pipeline */
        next_pipeline = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                        L3_PIPELINE_TELNET_TO_SERIAL :
                        L3_PIPELINE_SERIAL_TO_TELNET;

        /* Reset timeslice counter for new pipeline */
        l3_pipeline_t *next = (next_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                             &l3_ctx->pipeline_serial_to_telnet :
                             &l3_ctx->pipeline_telnet_to_serial;
        next->bytes_in_timeslice = 0;
        next->last_timeslice_start = time(NULL);

        l3_ctx->round_robin_counter++;
        MB_LOG_DEBUG("Fair scheduling: switched to pipeline %s (round #%d)",
                    l3_get_pipeline_name(next_pipeline), l3_ctx->round_robin_counter);
    }

    pthread_mutex_unlock(&l3_ctx->scheduling_mutex);
    return next_pipeline;
}

/* ========== Quantum Management Functions ========== */

/**
 * Process pipeline with quantum enforcement and latency tracking
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction to process
 * @return SUCCESS on success, error code on failure
 */
int l3_process_pipeline_with_quantum(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Record quantum start time */
    long long quantum_start = l3_get_timestamp_ms();
    l3_ctx->quantum_state.start_time = quantum_start;
    l3_ctx->quantum_state.bytes_processed = 0;

    /* Get current quantum size */
    int quantum_ms = l3_ctx->quantum_state.current_quantum_ms;
    if (quantum_ms <= 0) {
        quantum_ms = l3_ctx->sched_config.base_quantum_ms;
    }

    MB_LOG_DEBUG("Processing %s with quantum: %dms", l3_get_direction_name(direction), quantum_ms);

    /* Process for the duration of the quantum */
    long long elapsed = 0;
    size_t bytes_processed = 0;
    int iterations = 0;

    while (elapsed < quantum_ms) {
        l3_pipeline_t *pipeline = (direction == L3_PIPELINE_SERIAL_TO_TELNET) ?
                                 &l3_ctx->pipeline_serial_to_telnet :
                                 &l3_ctx->pipeline_telnet_to_serial;

        /* Check if pipeline has data */
        size_t available = l3_double_buffer_available(&pipeline->buffers);
        if (available == 0) {
            MB_LOG_DEBUG("Pipeline %s empty after %lldms", l3_get_direction_name(direction), elapsed);
            break;
        }

        /* Process a chunk of data */
        size_t chunk_size = (available < 256) ? available : 256;  /* Process up to 256 bytes at a time */

        /* TODO: Actually process the data here - this is a placeholder */
        /* In real implementation, this would call the appropriate processing function */

        bytes_processed += chunk_size;
        iterations++;

        /* Check elapsed time */
        elapsed = l3_get_timestamp_ms() - quantum_start;

        /* Check for forced switch conditions */
        if (l3_should_force_direction_switch(l3_ctx, direction == L3_PIPELINE_SERIAL_TO_TELNET ?
                                            L3_PIPELINE_TELNET_TO_SERIAL : L3_PIPELINE_SERIAL_TO_TELNET)) {
            MB_LOG_DEBUG("Forced switch after %lldms", elapsed);
            break;
        }
    }

    /* Update quantum state */
    l3_ctx->quantum_state.bytes_processed = bytes_processed;

    /* Update latency statistics */
    if (elapsed > 0) {
        l3_update_latency_stats(l3_ctx, direction, elapsed);
    }

    /* Update starvation tracking */
    long long current_time = l3_get_timestamp_ms();
    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        l3_ctx->sched_state.serial_starvation_time = current_time;
    } else {
        l3_ctx->sched_state.telnet_starvation_time = current_time;
    }

    MB_LOG_DEBUG("Quantum complete for %s: %lldms, %zu bytes, %d iterations",
                l3_get_direction_name(direction), elapsed, bytes_processed, iterations);

    return L3_SUCCESS;
}

int l3_calculate_optimal_quantum(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Base quantum */
    int quantum = l3_ctx->sched_config.base_quantum_ms;

    /* Adjust based on system load */
    double avg_latency = (l3_ctx->latency_stats.serial_to_telnet_avg_ms +
                          l3_ctx->latency_stats.telnet_to_serial_avg_ms) / 2.0;

    if (avg_latency > 100.0) {
        /* High latency - reduce quantum */
        quantum = quantum * 0.75;
    } else if (avg_latency < 20.0) {
        /* Low latency - increase quantum for efficiency */
        quantum = quantum * 1.25;
    }

    /* Apply bounds */
    if (quantum < l3_ctx->sched_config.min_quantum_ms) {
        quantum = l3_ctx->sched_config.min_quantum_ms;
    } else if (quantum > l3_ctx->sched_config.max_quantum_ms) {
        quantum = l3_ctx->sched_config.max_quantum_ms;
    }

    l3_ctx->quantum_state.current_quantum_ms = quantum;

    MB_LOG_DEBUG("Optimal quantum calculated: %dms (avg_latency: %.2fms)", quantum, avg_latency);

    return L3_SUCCESS;
}

int l3_calculate_adaptive_quantum_with_latency(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Get current system state */
    l3_pipeline_t *serial_pipeline = &l3_ctx->pipeline_serial_to_telnet;
    l3_pipeline_t *telnet_pipeline = &l3_ctx->pipeline_telnet_to_serial;

    size_t serial_backlog = l3_double_buffer_available(&serial_pipeline->buffers);
    size_t telnet_backlog = l3_double_buffer_available(&telnet_pipeline->buffers);
    size_t total_backlog = serial_backlog + telnet_backlog;

    /* Base quantum calculation */
    int quantum = l3_ctx->sched_config.base_quantum_ms;

    /* Factor 1: Backlog pressure */
    if (total_backlog > 4096) {
        /* High backlog - use smaller quantum for responsiveness */
        quantum = quantum * 0.5;
        MB_LOG_DEBUG("High backlog detected (%zu bytes) - reducing quantum", total_backlog);
    } else if (total_backlog < 512) {
        /* Low backlog - use larger quantum for efficiency */
        quantum = quantum * 1.5;
        MB_LOG_DEBUG("Low backlog (%zu bytes) - increasing quantum", total_backlog);
    }

    /* Factor 2: Latency constraints */
    double max_latency = (l3_ctx->latency_stats.max_serial_to_telnet_ms >
                          l3_ctx->latency_stats.max_telnet_to_serial_ms) ?
                         l3_ctx->latency_stats.max_serial_to_telnet_ms :
                         l3_ctx->latency_stats.max_telnet_to_serial_ms;

    if (max_latency > 200.0) {
        /* Latency violation - aggressive reduction */
        quantum = l3_ctx->sched_config.min_quantum_ms;
        MB_LOG_WARNING("Latency violation (%.2fms) - using minimum quantum", max_latency);
    }

    /* Factor 3: Direction imbalance */
    double imbalance = (serial_backlog > telnet_backlog) ?
                      (double)serial_backlog / (telnet_backlog + 1) :
                      (double)telnet_backlog / (serial_backlog + 1);

    if (imbalance > 3.0) {
        /* Significant imbalance - reduce quantum for faster switching */
        quantum = quantum * 0.7;
        MB_LOG_DEBUG("Direction imbalance detected (ratio: %.2f) - adjusting quantum", imbalance);
    }

    /* Apply bounds */
    if (quantum < l3_ctx->sched_config.min_quantum_ms) {
        quantum = l3_ctx->sched_config.min_quantum_ms;
    } else if (quantum > l3_ctx->sched_config.max_quantum_ms) {
        quantum = l3_ctx->sched_config.max_quantum_ms;
    }

    /* Update quantum state */
    l3_ctx->quantum_state.current_quantum_ms = quantum;

    MB_LOG_DEBUG("Adaptive quantum with latency: %dms (backlog: %zu, max_latency: %.2fms, imbalance: %.2f)",
                quantum, total_backlog, max_latency, imbalance);

    return L3_SUCCESS;
}

/* ========== Latency Management Functions ========== */

/**
 * Update latency statistics
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction
 * @param latency_ms Measured latency in milliseconds
 */
void l3_update_latency_stats(l3_context_t *l3_ctx, l3_pipeline_direction_t direction, long long latency_ms)
{
    if (!l3_ctx || latency_ms < 0) {
        return;
    }

    /* Update per-direction statistics */
    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        /* Update average using exponential moving average */
        if (l3_ctx->latency_stats.serial_to_telnet_avg_ms == 0.0) {
            l3_ctx->latency_stats.serial_to_telnet_avg_ms = (double)latency_ms;
        } else {
            l3_ctx->latency_stats.serial_to_telnet_avg_ms =
                0.9 * l3_ctx->latency_stats.serial_to_telnet_avg_ms + 0.1 * latency_ms;
        }

        /* Update maximum */
        if (latency_ms > l3_ctx->latency_stats.max_serial_to_telnet_ms) {
            l3_ctx->latency_stats.max_serial_to_telnet_ms = (double)latency_ms;
        }
    } else {
        /* Update telnet to serial statistics */
        if (l3_ctx->latency_stats.telnet_to_serial_avg_ms == 0.0) {
            l3_ctx->latency_stats.telnet_to_serial_avg_ms = (double)latency_ms;
        } else {
            l3_ctx->latency_stats.telnet_to_serial_avg_ms =
                0.9 * l3_ctx->latency_stats.telnet_to_serial_avg_ms + 0.1 * latency_ms;
        }

        if (latency_ms > l3_ctx->latency_stats.max_telnet_to_serial_ms) {
            l3_ctx->latency_stats.max_telnet_to_serial_ms = (double)latency_ms;
        }
    }

    /* Update global statistics */
    l3_ctx->latency_stats.total_samples++;
    l3_ctx->latency_stats.last_measurement_time = l3_get_timestamp_ms();

    /* Log if latency is concerning */
    if (latency_ms > 100) {
        MB_LOG_DEBUG("High latency detected for %s: %lldms",
                    l3_get_direction_name(direction), latency_ms);
    }
}

int l3_enforce_latency_boundaries(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Check serial to telnet latency */
    int ret = l3_detect_latency_violation(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET);
    if (ret != L3_SUCCESS) {
        MB_LOG_WARNING("Latency violation detected for serial->telnet pipeline");

        /* Take corrective action */
        if (l3_ctx->sched_config.adaptive_quantum_enabled) {
            /* Reduce quantum to improve responsiveness */
            l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.min_quantum_ms;
            MB_LOG_INFO("Reduced quantum to minimum (%dms) due to latency violation",
                       l3_ctx->sched_config.min_quantum_ms);
        }

        /* Force priority to the affected direction */
        l3_update_direction_priorities(l3_ctx);
    }

    /* Check telnet to serial latency */
    ret = l3_detect_latency_violation(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL);
    if (ret != L3_SUCCESS) {
        MB_LOG_WARNING("Latency violation detected for telnet->serial pipeline");

        /* Similar corrective actions */
        if (l3_ctx->sched_config.adaptive_quantum_enabled) {
            l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.min_quantum_ms;
        }

        l3_update_direction_priorities(l3_ctx);
    }

    /* Recalculate optimal quantum if adaptive mode is enabled */
    if (l3_ctx->sched_config.adaptive_quantum_enabled) {
        l3_calculate_adaptive_quantum_with_latency(l3_ctx);
    }

    return L3_SUCCESS;
}

int l3_detect_latency_violation(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Define latency bounds (in milliseconds) */
    const double WARNING_THRESHOLD = 100.0;
    const double ERROR_THRESHOLD = 200.0;

    double current_latency;
    double max_latency;

    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        current_latency = l3_ctx->latency_stats.serial_to_telnet_avg_ms;
        max_latency = l3_ctx->latency_stats.max_serial_to_telnet_ms;
    } else {
        current_latency = l3_ctx->latency_stats.telnet_to_serial_avg_ms;
        max_latency = l3_ctx->latency_stats.max_telnet_to_serial_ms;
    }

    /* Check for violations */
    if (max_latency > ERROR_THRESHOLD) {
        MB_LOG_ERROR("Critical latency violation for %s: max=%.2fms (threshold=%.2fms)",
                    l3_get_direction_name(direction), max_latency, ERROR_THRESHOLD);
        return L3_SUCCESS;  /* Violation detected */
    } else if (current_latency > WARNING_THRESHOLD) {
        MB_LOG_WARNING("Latency warning for %s: avg=%.2fms (threshold=%.2fms)",
                      l3_get_direction_name(direction), current_latency, WARNING_THRESHOLD);
        return L3_SUCCESS;  /* Violation detected (warning level) */
    }

    return L3_ERROR_NO_VIOLATION;  /* No violation */
}

long long l3_get_direction_wait_time(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return 0;
    }

    long long current_time = l3_get_timestamp_ms();
    long long wait_time = 0;

    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        /* Time since serial pipeline was last serviced */
        wait_time = current_time - l3_ctx->sched_state.serial_starvation_time;
    } else {
        /* Time since telnet pipeline was last serviced */
        wait_time = current_time - l3_ctx->sched_state.telnet_starvation_time;
    }

    /* Sanity check */
    if (wait_time < 0) {
        MB_LOG_WARNING("Negative wait time detected for %s: %lldms",
                      l3_get_direction_name(direction), wait_time);
        wait_time = 0;
    }

    return wait_time;
}

/* ========== Starvation Prevention Functions ========== */

/**
 * Check if direction is starving
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction
 * @return true if starving, false otherwise
 */
bool l3_is_direction_starving(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return false;
    }

    long long wait_time = l3_get_direction_wait_time(l3_ctx, direction);

    /* Check against starvation threshold */
    if (wait_time > l3_ctx->sched_config.starvation_threshold_ms) {
        MB_LOG_DEBUG("Direction %s is starving (wait_time: %lldms, threshold: %dms)",
                    l3_get_direction_name(direction), wait_time,
                    l3_ctx->sched_config.starvation_threshold_ms);
        return true;
    }

    return false;
}

bool l3_should_force_direction_switch(l3_context_t *l3_ctx, l3_pipeline_direction_t direction)
{
    if (!l3_ctx) {
        return false;
    }

    /* Check if the other direction is starving */
    long long wait_time = l3_get_direction_wait_time(l3_ctx, direction);

    /* Use adaptive threshold based on current system state */
    int latency_bound = l3_ctx->sched_config.starvation_threshold_ms / 3;  /* More aggressive */

    /* Force switch if wait time exceeds 150% of latency bound */
    if (wait_time > (latency_bound * 3 / 2)) {
        MB_LOG_DEBUG("Force switch condition met for %s: wait_time=%lldms, threshold=%dms",
                    l3_get_direction_name(direction), wait_time, (latency_bound * 3 / 2));
        return true;
    }

    /* Also consider starvation conditions */
    if (l3_is_direction_starving(l3_ctx, direction)) {
        MB_LOG_DEBUG("Force switch due to starvation for %s", l3_get_direction_name(direction));
        return true;
    }

    return false;
}

/* ========== Fair Queue Management Functions ========== */

/**
 * Update fair queue weights based on traffic patterns
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_update_fair_queue_weights(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Calculate current load on each direction */
    l3_pipeline_t *serial_pipeline = &l3_ctx->pipeline_serial_to_telnet;
    l3_pipeline_t *telnet_pipeline = &l3_ctx->pipeline_telnet_to_serial;

    size_t serial_backlog = l3_double_buffer_available(&serial_pipeline->buffers);
    size_t telnet_backlog = l3_double_buffer_available(&telnet_pipeline->buffers);

    /* Adjust weights based on backlog */
    if (serial_backlog > telnet_backlog * 2) {
        /* Serial has more data - increase its weight */
        l3_ctx->fair_queue.serial_weight = 7;
        l3_ctx->fair_queue.telnet_weight = 3;
    } else if (telnet_backlog > serial_backlog * 2) {
        /* Telnet has more data - increase its weight */
        l3_ctx->fair_queue.serial_weight = 3;
        l3_ctx->fair_queue.telnet_weight = 7;
    } else {
        /* Balanced load - equal weights */
        l3_ctx->fair_queue.serial_weight = 5;
        l3_ctx->fair_queue.telnet_weight = 5;
    }

    MB_LOG_DEBUG("Updated fair queue weights - serial: %d, telnet: %d (backlog: %zu/%zu)",
                l3_ctx->fair_queue.serial_weight, l3_ctx->fair_queue.telnet_weight,
                serial_backlog, telnet_backlog);

    return L3_SUCCESS;
}

int l3_update_direction_priorities(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Check starvation conditions */
    bool serial_starving = l3_is_direction_starving(l3_ctx, L3_PIPELINE_SERIAL_TO_TELNET);
    bool telnet_starving = l3_is_direction_starving(l3_ctx, L3_PIPELINE_TELNET_TO_SERIAL);

    /* Immediate priority adjustment for starving direction */
    if (serial_starving && !telnet_starving) {
        l3_ctx->sched_state.current_direction = L3_PIPELINE_SERIAL_TO_TELNET;
        l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.max_quantum_ms;
        MB_LOG_INFO("Priority boost for starving serial->telnet pipeline");
    } else if (telnet_starving && !serial_starving) {
        l3_ctx->sched_state.current_direction = L3_PIPELINE_TELNET_TO_SERIAL;
        l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.max_quantum_ms;
        MB_LOG_INFO("Priority boost for starving telnet->serial pipeline");
    } else if (serial_starving && telnet_starving) {
        /* Both starving - emergency mode */
        l3_ctx->quantum_state.current_quantum_ms = l3_ctx->sched_config.min_quantum_ms;
        MB_LOG_WARNING("Both directions starving - using minimum quantum for rapid switching");
    }

    /* Update fair queue weights based on priorities */
    l3_update_fair_queue_weights(l3_ctx);

    return L3_SUCCESS;
}

/* ========== Scheduling Statistics Functions ========== */

/**
 * Get scheduling statistics
 * @param l3_ctx Level 3 context
 * @param stats Output statistics structure
 * @return SUCCESS on success, error code on failure
 */
int l3_get_scheduling_statistics(l3_context_t *l3_ctx, l3_scheduling_stats_t *stats)
{
    if (!l3_ctx || !stats) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Clear output structure */
    memset(stats, 0, sizeof(l3_scheduling_stats_t));

    /* Copy latency statistics */
    stats->avg_latency_ms[0] = l3_ctx->latency_stats.serial_to_telnet_avg_ms;
    stats->avg_latency_ms[1] = l3_ctx->latency_stats.telnet_to_serial_avg_ms;
    stats->max_latency_samples[0] = (int)l3_ctx->latency_stats.max_serial_to_telnet_ms;
    stats->max_latency_samples[1] = (int)l3_ctx->latency_stats.max_telnet_to_serial_ms;
    stats->latency_exceedances[0] = 0;  /* Could be tracked if needed */
    stats->latency_exceedances[1] = 0;

    /* Copy byte statistics */
    stats->bytes_processed[0] = l3_ctx->pipeline_serial_to_telnet.total_bytes_processed;
    stats->bytes_processed[1] = l3_ctx->pipeline_telnet_to_serial.total_bytes_processed;
    stats->quantum_count[0] = 0;  /* Could be tracked if needed */
    stats->quantum_count[1] = 0;
    stats->avg_quantum_size[0] = 0;  /* Could be calculated if needed */
    stats->avg_quantum_size[1] = 0;

    /* Copy fairness statistics */
    stats->consecutive_slices[0] = l3_ctx->sched_state.consecutive_slices;
    stats->consecutive_slices[1] = 0;  /* Other direction */
    stats->forced_slices[0] = 0;  /* Could be tracked if needed */
    stats->forced_slices[1] = 0;
    stats->starvations_detected[0] = 0;  /* Could be tracked if needed */
    stats->starvations_detected[1] = 0;

    /* Calculate performance metrics */
    stats->fairness_ratio = 1.0;  /* Could be calculated based on actual metrics */
    stats->system_utilization = l3_get_system_utilization(l3_ctx) / 100.0;
    stats->total_scheduling_cycles = l3_ctx->round_robin_counter;
    stats->last_update_time = time(NULL);

    return L3_SUCCESS;
}

/* ========== Half-duplex Control Functions ========== */

int l3_switch_active_pipeline(l3_context_t *l3_ctx, l3_pipeline_direction_t new_active_pipeline)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (!l3_can_switch_pipeline(l3_ctx)) {
        MB_LOG_DEBUG("Pipeline switch not allowed at this time");
        return L3_ERROR_BUSY;
    }

    l3_ctx->active_pipeline = new_active_pipeline;
    l3_ctx->last_pipeline_switch = time(NULL);
    l3_ctx->total_pipeline_switches++;

    MB_LOG_DEBUG("Pipeline switch: %s (switch #%llu)",
                l3_get_pipeline_name(new_active_pipeline),
                (unsigned long long)l3_ctx->total_pipeline_switches);

    return L3_SUCCESS;
}

bool l3_can_switch_pipeline(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return false;
    }

    /* Don't switch if not in half-duplex mode */
    if (!l3_ctx->half_duplex_mode) {
        return false;
    }

    /* Check minimum time between switches */
    time_t now = time(NULL);
    if (now - l3_ctx->last_pipeline_switch < 1) {  /* At least 1 second between switches */
        return false;
    }

    /* Check if current pipeline is in the middle of critical operation */
    l3_pipeline_t *current = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                             &l3_ctx->pipeline_serial_to_telnet :
                             &l3_ctx->pipeline_telnet_to_serial;

    if (current->state == L3_PIPELINE_STATE_ERROR) {
        return false;
    }

    return true;
}