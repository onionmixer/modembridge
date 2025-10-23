/*
 * level3_state.h - State machine functions for Level 3 Pipeline Management
 *
 * This file contains declarations for state machine functions
 * extracted from level3.c for better modularity.
 */

#ifndef MODEMBRIDGE_LEVEL3_STATE_H
#define MODEMBRIDGE_LEVEL3_STATE_H

#include "level3_types.h"
#include <stdbool.h>
#include <time.h>

/* Note: This header requires level3.h to be included first for l3_context_t definition */
/* l3_result_t is defined in level3_types.h */
/* l3_context_t is defined in level3.h as an anonymous struct typedef */

/* ========== State Machine Core Functions ========== */

/**
 * Process the Level 3 state machine
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
l3_result_t l3_process_state_machine(l3_context_t *l3_ctx);

/**
 * Set the system state with optional timeout
 * @param l3_ctx Level 3 context
 * @param new_state New system state to transition to
 * @param timeout_seconds Timeout in seconds (0 = no timeout)
 * @return L3_SUCCESS on success, error code on failure
 */
l3_result_t l3_set_system_state(l3_context_t *l3_ctx, l3_system_state_t new_state, int timeout_seconds);

/**
 * Check if a state transition is valid
 * @param from_state Current state
 * @param to_state Target state
 * @return true if transition is valid, false otherwise
 */
bool l3_is_valid_state_transition(l3_system_state_t from_state, l3_system_state_t to_state);

/* ========== State Timeout Functions ========== */

/**
 * Handle state timeout
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
l3_result_t l3_handle_state_timeout(l3_context_t *l3_ctx);

/**
 * Check if current state has timed out
 * @param l3_ctx Level 3 context
 * @return true if state has timed out, false otherwise
 */
bool l3_is_state_timed_out(l3_context_t *l3_ctx);

/* ========== State Utility Functions ========== */

/**
 * Convert system state to string representation
 * @param state System state
 * @return String representation of the state
 */
const char *l3_system_state_to_string(l3_system_state_t state);

/* ========== DCD State Functions ========== */

/**
 * Get the current DCD (Data Carrier Detect) state
 * @param l3_ctx Level 3 context
 * @return true if DCD is active, false otherwise
 */
bool l3_get_dcd_state(l3_context_t *l3_ctx);

/**
 * Handle DCD rising edge event
 * Called from Level 1 modem when carrier is detected
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
l3_result_t l3_on_dcd_rising(l3_context_t *l3_ctx);

/**
 * Handle DCD falling edge event
 * Called from Level 1 modem when carrier is lost
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, error code on failure
 */
l3_result_t l3_on_dcd_falling(l3_context_t *l3_ctx);

/* ========== Global State Variables (for internal use) ========== */

/* Global state for telnet connection attempts in CONNECTING state */
extern bool g_level3_connection_attempted;
extern time_t g_level3_last_attempt;
extern bool g_level3_transition_logged;

#endif /* MODEMBRIDGE_LEVEL3_STATE_H */