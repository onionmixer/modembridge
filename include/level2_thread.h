/*
 * level2_thread.h - Level 2 (Telnet) Thread Functions
 *
 * This header declares the telnet thread function and related operations
 * for Level 2. The telnet thread handles all telnet I/O operations including:
 * - Reading from telnet server
 * - Writing to telnet server
 * - Processing IAC protocol sequences
 * - Managing bidirectional data flow
 */

#ifndef LEVEL2_THREAD_H
#define LEVEL2_THREAD_H

#ifdef ENABLE_LEVEL2

/* Forward declaration to avoid circular dependency */
#ifndef BRIDGE_CTX_DEFINED
#define BRIDGE_CTX_DEFINED
typedef struct bridge_ctx bridge_ctx_t;
#endif

/**
 * Telnet thread function (Level 2)
 *
 * This is the main thread function for Level 2 telnet operations.
 * It handles:
 * - Telnet connection management
 * - Bidirectional data transfer between serial and telnet
 * - IAC protocol processing
 * - Buffer management for thread-safe operation
 *
 * The thread runs continuously while ctx->thread_running is true.
 * It processes data in both directions:
 * 1. Telnet → Serial: Receives from telnet, processes IAC, sends to serial
 * 2. Serial → Telnet: Reads from buffer, sends to telnet
 *
 * @param arg Pointer to bridge_ctx_t structure
 * @return NULL on thread exit
 */
void *telnet_thread_func(void *arg);

#endif /* ENABLE_LEVEL2 */

#endif /* LEVEL2_THREAD_H */