/*
 * level3_schedule.h - Scheduling functions for Level 3 Pipeline Management
 *
 * This file contains declarations for scheduling and fairness functions
 * extracted from level3.c for better modularity.
 */

#ifndef MODEMBRIDGE_LEVEL3_SCHEDULE_H
#define MODEMBRIDGE_LEVEL3_SCHEDULE_H

#include "level3_types.h"
#include <stdbool.h>
#include <time.h>

/* Note: This header requires level3.h to be included first for l3_context_t definition */
/* l3_result_t and other types are defined in level3_types.h */

/* ========== Pipeline Scheduling Functions ========== */

/**
 * Initialize enhanced scheduling system with quantum enforcement
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_init_enhanced_scheduling(l3_context_t *l3_ctx);

/**
 * Schedule the next pipeline to process
 * @param l3_ctx Level 3 context
 * @return Pipeline direction to process next
 */
l3_pipeline_direction_t l3_schedule_next_pipeline(l3_context_t *l3_ctx);

/**
 * Switch the active pipeline
 * @param l3_ctx Level 3 context
 * @param new_active_pipeline New pipeline direction to activate
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_switch_active_pipeline(l3_context_t *l3_ctx, l3_pipeline_direction_t new_active_pipeline);

/**
 * Check if pipeline can be switched
 * @param l3_ctx Level 3 context
 * @return true if pipeline can be switched, false otherwise
 */
bool l3_can_switch_pipeline(l3_context_t *l3_ctx);

/* ========== Quantum Management Functions ========== */

/**
 * Process pipeline with quantum enforcement and latency tracking
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction to process
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_process_pipeline_with_quantum(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);

/**
 * Calculate optimal quantum based on system state
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_calculate_optimal_quantum(l3_context_t *l3_ctx);

/**
 * Calculate adaptive quantum with latency constraints
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_calculate_adaptive_quantum_with_latency(l3_context_t *l3_ctx);

/* ========== Latency Management Functions ========== */

/**
 * Update latency statistics
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction
 * @param latency_ms Measured latency in milliseconds
 */
void l3_update_latency_stats(l3_context_t *l3_ctx, l3_pipeline_direction_t direction, long long latency_ms);

/**
 * Enforce latency boundaries
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_enforce_latency_boundaries(l3_context_t *l3_ctx);

/**
 * Detect latency violation
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction to check
 * @return L3_SUCCESS if no violation, error code if violation detected
 */
int l3_detect_latency_violation(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);

/**
 * Get direction wait time
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction
 * @return Wait time in milliseconds
 */
long long l3_get_direction_wait_time(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);

/* ========== Starvation Prevention Functions ========== */

/**
 * Check if a direction is starving
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction to check
 * @return true if direction is starving, false otherwise
 */
bool l3_is_direction_starving(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);

/**
 * Check if direction switch should be forced
 * @param l3_ctx Level 3 context
 * @param direction Pipeline direction
 * @return true if switch should be forced, false otherwise
 */
bool l3_should_force_direction_switch(l3_context_t *l3_ctx, l3_pipeline_direction_t direction);

/* ========== Fair Queue Management Functions ========== */

/**
 * Update fair queue weights
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_update_fair_queue_weights(l3_context_t *l3_ctx);

/**
 * Update direction priorities
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_update_direction_priorities(l3_context_t *l3_ctx);

/* ========== Scheduling Statistics Functions ========== */

/**
 * Get scheduling statistics
 * @param l3_ctx Level 3 context
 * @param stats Output structure for statistics
 * @return L3_SUCCESS on success, error code on failure
 */
int l3_get_scheduling_statistics(l3_context_t *l3_ctx, l3_scheduling_stats_t *stats);

#endif /* MODEMBRIDGE_LEVEL3_SCHEDULE_H */