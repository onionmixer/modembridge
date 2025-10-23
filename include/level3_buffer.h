/*
 * level3_buffer.h - Buffer management functions for Level 3 Pipeline Management
 *
 * This file contains declarations for buffer management functions
 * extracted from level3.c for better modularity.
 */

#ifndef MODEMBRIDGE_LEVEL3_BUFFER_H
#define MODEMBRIDGE_LEVEL3_BUFFER_H

#include "level3_types.h"
#include "level3.h"  /* For structure definitions */
#include <stddef.h>
#include <stdbool.h>

/* ========== Double Buffer Management Functions ========== */

/**
 * Initialize double buffer structure
 * @param dbuf Double buffer to initialize
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_double_buffer_init(l3_double_buffer_t *dbuf);

/**
 * Write data to active sub-buffer
 * @param dbuf Double buffer context
 * @param data Data to write
 * @param len Data length
 * @return Number of bytes written (may be 0 if full)
 */
size_t l3_double_buffer_write(l3_double_buffer_t *dbuf, const unsigned char *data, size_t len);

/**
 * Read data from active main buffer
 * @param dbuf Double buffer context
 * @param data Output buffer
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
size_t l3_double_buffer_read(l3_double_buffer_t *dbuf, unsigned char *data, size_t len);

/**
 * Get available data in main buffer
 * @param dbuf Double buffer context
 * @return Number of bytes available
 */
size_t l3_double_buffer_available(l3_double_buffer_t *dbuf);

/**
 * Get free space in sub-buffer
 * @param dbuf Double buffer context
 * @return Number of bytes free
 */
size_t l3_double_buffer_free(l3_double_buffer_t *dbuf);

/* ========== Enhanced Double Buffer Management Functions ========== */

/**
 * Initialize enhanced double buffer with watermark defense
 * @param ebuf Enhanced double buffer to initialize
 * @param initial_size Initial buffer size
 * @param min_size Minimum buffer size
 * @param max_size Maximum buffer size
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_enhanced_double_buffer_init(l3_enhanced_double_buffer_t *ebuf,
                                   size_t initial_size, size_t min_size, size_t max_size);

/**
 * Cleanup enhanced double buffer and free memory
 * @param ebuf Enhanced double buffer to cleanup
 */
void l3_enhanced_double_buffer_cleanup(l3_enhanced_double_buffer_t *ebuf);

/**
 * Write data to enhanced buffer with watermark protection
 * @param ebuf Enhanced double buffer context
 * @param data Data to write
 * @param len Data length
 * @return Number of bytes written (may be 0 if full or backpressure)
 */
size_t l3_enhanced_double_buffer_write(l3_enhanced_double_buffer_t *ebuf,
                                       const unsigned char *data, size_t len);

/**
 * Read data from enhanced buffer
 * @param ebuf Enhanced double buffer context
 * @param data Output buffer
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
size_t l3_enhanced_double_buffer_read(l3_enhanced_double_buffer_t *ebuf,
                                      unsigned char *data, size_t len);

/**
 * Get current watermark level for buffer
 * @param ebuf Enhanced double buffer context
 * @return Current watermark level
 */
l3_watermark_level_t l3_get_watermark_level(l3_enhanced_double_buffer_t *ebuf);

/**
 * Resize enhanced buffer dynamically
 * @param ebuf Enhanced double buffer context
 * @param new_size New buffer size
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_resize_buffer(l3_enhanced_double_buffer_t *ebuf, size_t new_size);

/**
 * Update buffer metrics after operation
 * @param ebuf Enhanced double buffer context
 * @param bytes_written Number of bytes written (0 if read operation)
 * @param bytes_read Number of bytes read (0 if write operation)
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_update_buffer_metrics(l3_enhanced_double_buffer_t *ebuf,
                             size_t bytes_written, size_t bytes_read);

/**
 * Get comprehensive buffer statistics
 * @param ebuf Enhanced double buffer context
 * @param metrics Output structure for buffer metrics
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_get_buffer_metrics(l3_enhanced_double_buffer_t *ebuf, l3_buffer_metrics_t *metrics);

/**
 * Check and apply backpressure based on watermark level
 * @param ebuf Enhanced double buffer context
 * @return true if backpressure should be applied
 */
bool l3_should_apply_enhanced_backpressure(l3_enhanced_double_buffer_t *ebuf);

/* ========== Memory Pool Management Functions ========== */

/**
 * Initialize memory pool for fragmentation prevention
 * @param pool Memory pool to initialize
 * @param pool_size Total size of memory pool
 * @param block_size Size of individual blocks
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_memory_pool_init(l3_memory_pool_t *pool, size_t pool_size, size_t block_size);

/**
 * Allocate block from memory pool
 * @param pool Memory pool context
 * @return Pointer to allocated block, NULL if failed
 */
unsigned char *l3_memory_pool_alloc(l3_memory_pool_t *pool);

/**
 * Free block back to memory pool
 * @param pool Memory pool context
 * @param block Block to free (must be from this pool)
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
int l3_memory_pool_free(l3_memory_pool_t *pool, unsigned char *block);

/**
 * Cleanup memory pool and release all memory
 * @param pool Memory pool to cleanup
 */
void l3_memory_pool_cleanup(l3_memory_pool_t *pool);

/* ========== Pipeline Buffer Management Functions ========== */

/**
 * Check if backpressure should be applied to a pipeline
 * @param pipeline Pipeline to check
 * @return true if backpressure should be applied
 */
bool l3_should_apply_backpressure(l3_pipeline_t *pipeline);

#endif /* MODEMBRIDGE_LEVEL3_BUFFER_H */