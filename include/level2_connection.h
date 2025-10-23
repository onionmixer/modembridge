/*
 * level2_connection.h - Level 2 (Telnet) Connection Management
 *
 * This header declares functions for managing telnet connections
 * including connection establishment, disconnection, and state management.
 */

#ifndef LEVEL2_CONNECTION_H
#define LEVEL2_CONNECTION_H

#ifdef ENABLE_LEVEL2

/* Forward declaration to avoid circular dependency */
#ifndef BRIDGE_CTX_DEFINED
#define BRIDGE_CTX_DEFINED
typedef struct bridge_ctx bridge_ctx_t;
#endif

/**
 * Handle telnet connection establishment (Level 2 only)
 *
 * Called when a telnet connection is successfully established.
 * Updates bridge state to connected and performs any necessary
 * initialization for the active connection.
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, ERROR_INVALID_ARG if ctx is NULL
 */
int bridge_handle_telnet_connect(bridge_ctx_t *ctx);

/**
 * Handle telnet disconnection (Level 2 only)
 *
 * Called when a telnet connection is lost or closed.
 * Performs cleanup including:
 * - Hanging up the modem if online
 * - Sending NO CARRIER to the modem client
 * - Reinitializing the modem to initial state
 * - Updating bridge state to idle
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, ERROR_INVALID_ARG if ctx is NULL
 */
int bridge_handle_telnet_disconnect(bridge_ctx_t *ctx);

/**
 * Handle modem disconnection (Level 2 only)
 *
 * Called when the modem disconnects while a telnet connection exists.
 * Performs cleanup including:
 * - Disconnecting the telnet connection
 * - Sending NO CARRIER to modem
 * - Reinitializing the modem to initial state
 *
 * @param ctx Bridge context
 * @return SUCCESS on success, ERROR_INVALID_ARG if ctx is NULL
 */
int bridge_handle_modem_disconnect(bridge_ctx_t *ctx);

/**
 * Synchronize echo mode between telnet and modem (Level 2 only)
 *
 * Coordinates echo settings between the telnet server and modem client
 * to prevent double echo. If the telnet server will echo (TELOPT_ECHO),
 * disables modem local echo. Otherwise, uses the modem's echo setting
 * as configured by ATE command.
 *
 * @param ctx Bridge context
 */
void bridge_sync_echo_mode(bridge_ctx_t *ctx);

#endif /* ENABLE_LEVEL2 */

#endif /* LEVEL2_CONNECTION_H */