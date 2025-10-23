/*
 * level2_transfer.h - Level 2 (Telnet) Data Transfer Functions
 *
 * This header declares functions for transferring data between
 * telnet and serial interfaces, including protocol processing.
 */

#ifndef LEVEL2_TRANSFER_H
#define LEVEL2_TRANSFER_H

#ifdef ENABLE_LEVEL2

/* Forward declaration to avoid circular dependency */
#ifndef BRIDGE_CTX_DEFINED
#define BRIDGE_CTX_DEFINED
typedef struct bridge_ctx bridge_ctx_t;
#endif

/**
 * Process data from telnet (Level 2 only)
 *
 * Main entry point for processing telnet data.
 * Checks if telnet is connected and modem is online, then
 * transfers data from telnet to serial if conditions are met.
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, error code on failure
 */
int bridge_process_telnet_data(bridge_ctx_t *ctx);

/**
 * Transfer data from telnet to serial (Level 2 only)
 *
 * Handles the complete data flow from telnet to serial:
 * 1. Receives data from telnet server
 * 2. Processes IAC protocol sequences
 * 3. Passes through ANSI sequences
 * 4. Sends processed data to serial port
 * 5. Updates statistics
 *
 * This function is called from bridge_process_telnet_data when
 * both telnet and modem connections are active.
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, ERROR_CONNECTION on connection error,
 *         ERROR_IO on I/O error
 */
int bridge_transfer_telnet_to_serial(bridge_ctx_t *ctx);

#endif /* ENABLE_LEVEL2 */

#endif /* LEVEL2_TRANSFER_H */