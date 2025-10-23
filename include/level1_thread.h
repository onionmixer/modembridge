/*
 * level1_thread.h - Serial/Modem thread functions for Level 1
 *
 * This file contains declarations for the Serial/Modem thread functionality
 * specific to Level 1 (Serial/Modem only) operation. This thread handles
 * all serial I/O, modem management, health checks, and timestamp operations.
 */

#ifndef MODEMBRIDGE_LEVEL1_THREAD_H
#define MODEMBRIDGE_LEVEL1_THREAD_H

#include "common.h"
#include <pthread.h>

/* Forward declaration (bridge.h is included by the implementation) */
#ifndef BRIDGE_CTX_DEFINED
struct bridge_ctx;
typedef struct bridge_ctx bridge_ctx_t;
#endif

/* ========== Thread Functions ========== */

#ifdef ENABLE_LEVEL1

/**
 * Serial/Modem thread main function
 *
 * This is the main thread function for Level 1 operations.
 * It handles:
 * - Serial port health checks
 * - Modem initialization verification
 * - Timestamp transmission when online
 * - Echo functionality coordination
 * - Serial to telnet data transfer (when Level 2 is enabled)
 * - Telnet to serial data transfer (when Level 2 is enabled)
 *
 * The thread runs continuously while ctx->thread_running is true.
 * It performs health checks once at startup and then manages
 * bidirectional data flow between serial and telnet interfaces.
 *
 * @param arg Pointer to bridge_ctx_t
 * @return NULL on thread exit
 */
void *serial_modem_thread_func(void *arg);

#endif /* ENABLE_LEVEL1 */

#endif /* MODEMBRIDGE_LEVEL1_THREAD_H */