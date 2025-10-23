/*
 * level1_serial.h - Serial data processing functions for Level 1
 *
 * This file contains declarations for serial data processing functions
 * specific to Level 1 (Serial/Modem only) operation. These functions handle
 * direct serial communication without telnet involvement.
 */

#ifndef MODEMBRIDGE_LEVEL1_SERIAL_H
#define MODEMBRIDGE_LEVEL1_SERIAL_H

#include "common.h"
#include <stdbool.h>

/* Forward declaration (bridge.h is included by the implementation) */
#ifndef BRIDGE_CTX_DEFINED
struct bridge_ctx;
typedef struct bridge_ctx bridge_ctx_t;
#endif

/* ========== Serial Data Processing Functions ========== */

#ifdef ENABLE_LEVEL1

/*
 * Note: bridge_process_serial_data() is declared in bridge.h
 * The Level 1 implementation is provided in level1_serial.c
 */

/**
 * Handle modem connect event - Level 1 version
 *
 * In Level 1 mode, this function handles the modem going online
 * without establishing a telnet connection. It manages echo settings
 * and timestamp functionality.
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_modem_connect_level1(bridge_ctx_t *ctx);

/**
 * Handle modem disconnect event - Level 1 version
 *
 * In Level 1 mode, this function handles the modem going offline.
 * It updates state and manages cleanup without telnet involvement.
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_handle_modem_disconnect_level1(bridge_ctx_t *ctx);

/* ========== Helper Functions ========== */

/**
 * Process serial data in command mode
 *
 * Handles AT commands and modem responses when not in online mode.
 *
 * @param ctx Bridge context
 * @param buf Data buffer
 * @param len Data length
 * @return Number of bytes processed
 */
int level1_process_command_mode(bridge_ctx_t *ctx, unsigned char *buf, size_t len);

/**
 * Process serial data in online mode
 *
 * Handles data transfer when modem is online, including echo processing
 * and timestamp transmission.
 *
 * @param ctx Bridge context
 * @param buf Data buffer
 * @param len Data length
 * @return Number of bytes processed
 */
int level1_process_online_mode(bridge_ctx_t *ctx, unsigned char *buf, size_t len);

/**
 * Log serial data with hex dump
 *
 * Helper function to log serial data in both ASCII and hex format
 * for debugging purposes.
 *
 * @param buf Data buffer
 * @param len Data length
 * @param prefix Log message prefix
 */
void level1_log_serial_data(const unsigned char *buf, size_t len, const char *prefix);

/**
 * Handle serial I/O error
 *
 * Processes serial port errors, closes the port, and transitions
 * to disconnected state with retry scheduling.
 *
 * @param ctx Bridge context
 * @param error_code Error code from serial operation
 * @return ERROR_IO
 */
int level1_handle_serial_error(bridge_ctx_t *ctx, int error_code);

/**
 * Check and process hardware modem messages
 *
 * Checks for unsolicited messages from hardware modems (RING, CONNECT, NO CARRIER)
 * and processes them appropriately.
 *
 * @param ctx Bridge context
 * @param buf Data buffer
 * @param len Data length
 * @return true if a hardware message was processed, false otherwise
 */
bool level1_check_hardware_messages(bridge_ctx_t *ctx, const char *buf, size_t len);

/**
 * Initialize Level 1 serial processing
 *
 * Sets up Level 1 specific serial processing, including echo and timestamp
 * functionality initialization.
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int level1_serial_init(bridge_ctx_t *ctx);

/**
 * Cleanup Level 1 serial processing
 *
 * Performs cleanup for Level 1 serial processing, including
 * echo and timestamp functionality cleanup.
 *
 * @param ctx Bridge context
 */
void level1_serial_cleanup(bridge_ctx_t *ctx);

#endif /* ENABLE_LEVEL1 */

#endif /* MODEMBRIDGE_LEVEL1_SERIAL_H */