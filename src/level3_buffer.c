/*
 * level3_buffer.c - Buffer management functions for Level 3 Pipeline Management
 *
 * This file contains buffer management functions extracted from level3.c
 * for better modularity and maintainability.
 */

#include "level3_buffer.h"
#include "level3.h"
#include "common.h"
#include "level3_util.h"
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

/* ========== Double Buffer Management Functions ========== */

int l3_double_buffer_init(l3_double_buffer_t *dbuf)
{
    if (dbuf == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(dbuf, 0, sizeof(l3_double_buffer_t));

    /* Initialize buffer state */
    dbuf->main_active = true;
    dbuf->main_len = 0;
    dbuf->main_pos = 0;
    dbuf->sub_len = 0;

    /* Initialize synchronization */
    int ret = pthread_mutex_init(&dbuf->mutex, NULL);
    if (ret != 0) {
        MB_LOG_ERROR("Failed to initialize double buffer mutex: %s", strerror(ret));
        return L3_ERROR_THREAD;
    }

    return L3_SUCCESS;
}

size_t l3_double_buffer_write(l3_double_buffer_t *dbuf, const unsigned char *data, size_t len)
{
    if (dbuf == NULL || data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);

    /* Check if sub-buffer has space */
    size_t available_space = L3_PIPELINE_BUFFER_SIZE - dbuf->sub_len;
    size_t to_write = len < available_space ? len : available_space;

    if (to_write > 0) {
        memcpy(dbuf->sub_data + dbuf->sub_len, data, to_write);
        dbuf->sub_len += to_write;
        dbuf->last_activity = time(NULL);
    } else {
        /* Buffer overflow - drop data */
        dbuf->bytes_dropped += len;
        MB_LOG_WARNING("Double buffer overflow: dropped %zu bytes", len);
    }

    pthread_mutex_unlock(&dbuf->mutex);
    return to_write;
}

size_t l3_double_buffer_read(l3_double_buffer_t *dbuf, unsigned char *data, size_t len)
{
    if (dbuf == NULL || data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);

    /* Read from main buffer */
    size_t available = dbuf->main_len - dbuf->main_pos;
    size_t to_read = len < available ? len : available;

    if (to_read > 0) {
        memcpy(data, dbuf->main_data + dbuf->main_pos, to_read);
        dbuf->main_pos += to_read;
        dbuf->bytes_processed += to_read;
        dbuf->last_activity = time(NULL);
    }

    pthread_mutex_unlock(&dbuf->mutex);
    return to_read;
}

size_t l3_double_buffer_available(l3_double_buffer_t *dbuf)
{
    if (dbuf == NULL) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);
    size_t available = dbuf->main_len - dbuf->main_pos;
    pthread_mutex_unlock(&dbuf->mutex);

    return available;
}

size_t l3_double_buffer_free(l3_double_buffer_t *dbuf)
{
    if (dbuf == NULL) {
        return 0;
    }

    pthread_mutex_lock(&dbuf->mutex);
    size_t free_space = L3_PIPELINE_BUFFER_SIZE - dbuf->sub_len;
    pthread_mutex_unlock(&dbuf->mutex);

    return free_space;
}

/* ========== Enhanced Double Buffer Management Functions ========== */

int l3_enhanced_double_buffer_init(l3_enhanced_double_buffer_t *ebuf,
                                   size_t initial_size, size_t min_size, size_t max_size)
{
    if (!ebuf || initial_size == 0 || min_size == 0 || max_size == 0) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (min_size > max_size || initial_size < min_size || initial_size > max_size) {
        MB_LOG_ERROR("Invalid buffer size parameters: initial=%zu, min=%zu, max=%zu",
                    initial_size, min_size, max_size);
        return L3_ERROR_INVALID_PARAM;
    }

    memset(ebuf, 0, sizeof(l3_enhanced_double_buffer_t));

    /* Allocate buffers dynamically */
    ebuf->main_data = malloc(initial_size);
    ebuf->sub_data = malloc(initial_size);

    if (!ebuf->main_data || !ebuf->sub_data) {
        MB_LOG_ERROR("Failed to allocate enhanced buffer memory");
        free(ebuf->main_data);
        free(ebuf->sub_data);
        return L3_ERROR_MEMORY;
    }

    /* Initialize buffer configuration */
    ebuf->buffer_size = initial_size;
    ebuf->config.min_buffer_size = min_size;
    ebuf->config.max_buffer_size = max_size;
    ebuf->config.current_buffer_size = initial_size;

    /* Set default watermark levels */
    ebuf->config.critical_watermark = (size_t)(max_size * 0.95);
    ebuf->config.high_watermark = (size_t)(max_size * 0.80);
    ebuf->config.low_watermark = (size_t)(max_size * 0.20);
    ebuf->config.empty_watermark = (size_t)(max_size * 0.05);

    /* Enable adaptive features */
    ebuf->config.adaptive_sizing_enabled = true;
    ebuf->config.backpressure_enabled = true;
    ebuf->config.flow_control_enabled = true;

    /* Set adaptive sizing parameters */
    ebuf->config.growth_threshold = 85;      /* Grow when >85% full */
    ebuf->config.shrink_threshold = 15;      /* Shrink when <15% full */
    ebuf->config.growth_step_size = 1024;    /* Grow in 1KB steps */
    ebuf->config.shrink_step_size = 512;     /* Shrink in 512B steps */

    /* Initialize metrics */
    ebuf->metrics.current_usage = 0;
    ebuf->metrics.peak_usage = 0;
    ebuf->metrics.min_free_space = initial_size;
    ebuf->metrics.current_level = L3_WATERMARK_EMPTY;
    ebuf->metrics.peak_level = L3_WATERMARK_EMPTY;
    ebuf->metrics.avg_fill_ratio = 0.0;

    /* Initialize watermark state */
    ebuf->current_watermark = L3_WATERMARK_EMPTY;
    ebuf->watermark_change_time = time(NULL);
    ebuf->backpressure_active = false;

    /* Initialize dynamic sizing state */
    ebuf->last_resize_time = time(NULL);
    ebuf->consecutive_overflows = 0;
    ebuf->consecutive_underflows = 0;

    /* Initialize buffer switching */
    ebuf->main_active = true;
    ebuf->main_len = 0;
    ebuf->main_pos = 0;
    ebuf->sub_len = 0;

    /* Initialize mutex */
    if (pthread_mutex_init(&ebuf->mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize enhanced buffer mutex");
        free(ebuf->main_data);
        free(ebuf->sub_data);
        return L3_ERROR_THREAD;
    }

    /* Initialize memory pool */
    int ret = l3_memory_pool_init(&ebuf->memory_pool,
                                  max_size * 2,  /* Pool size = 2x max buffer */
                                  512);         /* Block size = 512 bytes */
    if (ret != L3_SUCCESS) {
        MB_LOG_WARNING("Failed to initialize memory pool, continuing without it");
    }

    MB_LOG_INFO("Enhanced buffer initialized: size=%zu, min=%zu, max=%zu",
                initial_size, min_size, max_size);

    return L3_SUCCESS;
}

void l3_enhanced_double_buffer_cleanup(l3_enhanced_double_buffer_t *ebuf)
{
    if (!ebuf) {
        return;
    }

    /* Cleanup memory pool */
    l3_memory_pool_cleanup(&ebuf->memory_pool);

    /* Free allocated buffers */
    free(ebuf->main_data);
    free(ebuf->sub_data);

    /* Cleanup mutex */
    pthread_mutex_destroy(&ebuf->mutex);

    memset(ebuf, 0, sizeof(l3_enhanced_double_buffer_t));

    MB_LOG_DEBUG("Enhanced buffer cleaned up");
}

l3_watermark_level_t l3_get_watermark_level(l3_enhanced_double_buffer_t *ebuf)
{
    if (!ebuf) {
        return L3_WATERMARK_EMPTY;
    }

    size_t total_usage = ebuf->main_len + ebuf->sub_len;
    double fill_ratio = (double)total_usage / (ebuf->buffer_size * 2);

    if (fill_ratio > 0.95) {
        return L3_WATERMARK_CRITICAL;
    } else if (fill_ratio > 0.80) {
        return L3_WATERMARK_HIGH;
    } else if (fill_ratio > 0.20) {
        return L3_WATERMARK_NORMAL;
    } else if (fill_ratio > 0.05) {
        return L3_WATERMARK_LOW;
    } else {
        return L3_WATERMARK_EMPTY;
    }
}

size_t l3_enhanced_double_buffer_write(l3_enhanced_double_buffer_t *ebuf,
                                       const unsigned char *data, size_t len)
{
    if (!ebuf || !data || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&ebuf->mutex);

    /* Check current watermark level */
    l3_watermark_level_t current_level = l3_get_watermark_level(ebuf);

    /* Enhanced backpressure check with hysteresis logic */
    if (ebuf->config.backpressure_enabled) {
        bool should_block = l3_should_apply_enhanced_backpressure(ebuf);

        if (should_block) {
            ebuf->metrics.overflow_events++;
            ebuf->consecutive_overflows++;
            ebuf->metrics.bytes_dropped += len;

            MB_LOG_DEBUG("Backpressure active - dropping %zu bytes (level: %s, overflow #%d)",
                        len, l3_watermark_level_to_string(current_level),
                        ebuf->consecutive_overflows);

            pthread_mutex_unlock(&ebuf->mutex);
            return 0;  /* Drop all data when backpressure active */
        }
    }

    /* Check available space in sub-buffer */
    size_t available_space = ebuf->buffer_size - ebuf->sub_len;
    size_t to_write = len < available_space ? len : available_space;

    if (to_write > 0) {
        memcpy(ebuf->sub_data + ebuf->sub_len, data, to_write);
        ebuf->sub_len += to_write;
        ebuf->last_activity = time(NULL);

        /* Reset consecutive overflows on successful write */
        ebuf->consecutive_overflows = 0;

        MB_LOG_DEBUG("Successfully wrote %zu bytes (level: %s)",
                    to_write, l3_watermark_level_to_string(current_level));
    } else {
        /* Buffer full - count as overflow even if not at critical level */
        ebuf->metrics.overflow_events++;
        ebuf->consecutive_overflows++;
        ebuf->metrics.bytes_dropped += len;

        MB_LOG_DEBUG("Buffer full - dropping %zu bytes (level: %s, overflow #%d)",
                    len, l3_watermark_level_to_string(current_level),
                    ebuf->consecutive_overflows);
    }

    /* Update watermark level and metrics */
    l3_watermark_level_t new_level = l3_get_watermark_level(ebuf);
    if (new_level != current_level) {
        ebuf->current_watermark = new_level;
        ebuf->watermark_change_time = time(NULL);
        MB_LOG_DEBUG("Watermark level changed: %s -> %s",
                    l3_watermark_level_to_string(current_level),
                    l3_watermark_level_to_string(new_level));
    }

    l3_update_buffer_metrics(ebuf, to_write, 0);

    pthread_mutex_unlock(&ebuf->mutex);
    return to_write;
}

size_t l3_enhanced_double_buffer_read(l3_enhanced_double_buffer_t *ebuf,
                                      unsigned char *data, size_t len)
{
    if (!ebuf || !data || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&ebuf->mutex);

    /* Read from main buffer */
    size_t available = ebuf->main_len - ebuf->main_pos;
    size_t to_read = len < available ? len : available;

    if (to_read > 0) {
        memcpy(data, ebuf->main_data + ebuf->main_pos, to_read);
        ebuf->main_pos += to_read;
        ebuf->bytes_processed += to_read;
        ebuf->last_activity = time(NULL);

        /* Reset consecutive underflows on successful read */
        ebuf->consecutive_underflows = 0;
    } else {
        /* No data available - count as underflow */
        ebuf->metrics.underflow_events++;
        ebuf->consecutive_underflows++;

        MB_LOG_DEBUG("Buffer empty - underflow #%d", ebuf->consecutive_underflows);
    }

    /* Update metrics */
    l3_update_buffer_metrics(ebuf, 0, to_read);

    pthread_mutex_unlock(&ebuf->mutex);
    return to_read;
}

bool l3_should_apply_enhanced_backpressure(l3_enhanced_double_buffer_t *ebuf)
{
    if (!ebuf || !ebuf->config.backpressure_enabled) {
        return false;
    }

    l3_watermark_level_t level = l3_get_watermark_level(ebuf);
    bool should_apply = false;

    /* BACKPRESSURE.txt compliant hysteresis logic:
     * - Apply backpressure at HIGH watermark (80%) to prevent overflow
     * - Release backpressure at LOW watermark (20%) to ensure recovery
     * This creates the necessary hysteresis to prevent oscillation
     */
    if (ebuf->backpressure_active) {
        /* Currently in backpressure - release only when below LOW watermark */
        should_apply = (level != L3_WATERMARK_LOW && level != L3_WATERMARK_EMPTY);

        if (!should_apply) {
            MB_LOG_DEBUG("Backpressure released: level=%s (below LOW watermark)",
                        l3_watermark_level_to_string(level));
        }
    } else {
        /* Currently not in backpressure - apply at HIGH or above */
        should_apply = (level == L3_WATERMARK_HIGH || level == L3_WATERMARK_CRITICAL);

        if (should_apply) {
            MB_LOG_DEBUG("Backpressure applied: level=%s (at HIGH watermark)",
                        l3_watermark_level_to_string(level));
        }
    }

    /* Update backpressure state if changed */
    if (should_apply != ebuf->backpressure_active) {
        ebuf->backpressure_active = should_apply;
        ebuf->watermark_change_time = time(NULL);

        MB_LOG_INFO("Backpressure state changed: %s -> %s (level: %s)",
                    should_apply ? "RELEASED" : "APPLIED",
                    should_apply ? "APPLIED" : "RELEASED",
                    l3_watermark_level_to_string(level));
    }

    return should_apply;
}

int l3_resize_buffer(l3_enhanced_double_buffer_t *ebuf, size_t new_size)
{
    if (!ebuf || new_size == 0) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Validate new size against configured bounds */
    if (new_size < ebuf->config.min_buffer_size || new_size > ebuf->config.max_buffer_size) {
        MB_LOG_ERROR("Invalid resize target: %zu bytes (min: %zu, max: %zu)",
                    new_size, ebuf->config.min_buffer_size, ebuf->config.max_buffer_size);
        return L3_ERROR_INVALID_PARAM;
    }

    /* No resize needed */
    if (new_size == ebuf->buffer_size) {
        MB_LOG_DEBUG("Resize not needed: already %zu bytes", new_size);
        return L3_SUCCESS;
    }

    MB_LOG_INFO("Resizing enhanced buffer: %zu -> %zu bytes", ebuf->buffer_size, new_size);

    /* Allocate new buffers */
    unsigned char *new_main_data = malloc(new_size);
    unsigned char *new_sub_data = malloc(new_size);

    if (!new_main_data || !new_sub_data) {
        MB_LOG_ERROR("Failed to allocate memory for buffer resize");
        free(new_main_data);
        free(new_sub_data);
        return L3_ERROR_MEMORY;
    }

    /* Copy existing data to new buffers */
    size_t main_copy_len = ebuf->main_len < new_size ? ebuf->main_len : new_size;
    size_t sub_copy_len = ebuf->sub_len < new_size ? ebuf->sub_len : new_size;

    if (main_copy_len > 0) {
        memcpy(new_main_data, ebuf->main_data, main_copy_len);
    }
    if (sub_copy_len > 0) {
        memcpy(new_sub_data, ebuf->sub_data, sub_copy_len);
    }

    /* Handle data truncation if shrinking */
    if (new_size < ebuf->buffer_size) {
        if (ebuf->main_len > new_size) {
            ebuf->main_len = new_size;
            MB_LOG_WARNING("Main buffer truncated: %zu -> %zu bytes", ebuf->main_len, new_size);
        }
        if (ebuf->sub_len > new_size) {
            ebuf->sub_len = new_size;
            MB_LOG_WARNING("Sub buffer truncated: %zu -> %zu bytes", ebuf->sub_len, new_size);
        }
        /* Adjust read position if beyond new buffer size */
        if (ebuf->main_pos >= new_size) {
            ebuf->main_pos = 0;  /* Reset to beginning if truncated past current position */
        }
    }

    /* Free old buffers and update pointers */
    free(ebuf->main_data);
    free(ebuf->sub_data);

    ebuf->main_data = new_main_data;
    ebuf->sub_data = new_sub_data;
    ebuf->buffer_size = new_size;
    ebuf->config.current_buffer_size = new_size;

    /* Update watermark thresholds based on new size */
    ebuf->config.critical_watermark = (size_t)(new_size * 0.95);
    ebuf->config.high_watermark = (size_t)(new_size * 0.80);
    ebuf->config.low_watermark = (size_t)(new_size * 0.20);
    ebuf->config.empty_watermark = (size_t)(new_size * 0.05);

    MB_LOG_INFO("Buffer resize completed: %zu bytes, watermarks updated (critical: %zu, high: %zu, low: %zu)",
                new_size, ebuf->config.critical_watermark, ebuf->config.high_watermark, ebuf->config.low_watermark);

    return L3_SUCCESS;
}

int l3_update_buffer_metrics(l3_enhanced_double_buffer_t *ebuf,
                             size_t bytes_written, size_t bytes_read)
{
    if (!ebuf) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Suppress unused parameter warnings */
    (void)bytes_written;
    (void)bytes_read;

    /* Update current usage */
    ebuf->metrics.current_usage = ebuf->main_len + ebuf->sub_len;

    /* Update peak usage */
    if (ebuf->metrics.current_usage > ebuf->metrics.peak_usage) {
        ebuf->metrics.peak_usage = ebuf->metrics.current_usage;
    }

    /* Update minimum free space */
    size_t total_capacity = ebuf->buffer_size * 2;
    size_t current_free = total_capacity - ebuf->metrics.current_usage;
    if (current_free < ebuf->metrics.min_free_space) {
        ebuf->metrics.min_free_space = current_free;
    }

    /* Update current level */
    ebuf->metrics.current_level = ebuf->current_watermark;

    /* Update peak level */
    if (ebuf->current_watermark > ebuf->metrics.peak_level) {
        ebuf->metrics.peak_level = ebuf->current_watermark;
        ebuf->metrics.time_at_peak_level = time(NULL);
    }

    /* Update average fill ratio */
    double current_fill_ratio = (double)ebuf->metrics.current_usage / total_capacity;
    if (ebuf->metrics.avg_fill_ratio == 0.0) {
        ebuf->metrics.avg_fill_ratio = current_fill_ratio;
    } else {
        /* Exponential moving average with alpha = 0.1 */
        ebuf->metrics.avg_fill_ratio = 0.9 * ebuf->metrics.avg_fill_ratio + 0.1 * current_fill_ratio;
    }

    ebuf->metrics.last_activity = time(NULL);

    /* BACKPRESSURE.txt compliant dynamic resizing */
    if (ebuf->config.adaptive_sizing_enabled) {
        /* Check if resizing is needed (rate-limited to prevent thrashing) */
        time_t now = time(NULL);
        if (now - ebuf->last_resize_time > 30) {  /* Minimum 30 seconds between resizes */
            bool should_grow = false;
            bool should_shrink = false;

            /* Growth condition: consistent high usage or frequent overflows */
            if (current_fill_ratio > 0.85 || ebuf->consecutive_overflows >= 3) {
                should_grow = true;
            }
            /* Shrink condition: consistent low usage */
            else if (current_fill_ratio < 0.15 && ebuf->buffer_size > ebuf->config.min_buffer_size) {
                should_shrink = true;
            }

            if (should_grow || should_shrink) {
                size_t new_size = ebuf->buffer_size;

                if (should_grow) {
                    /* Grow by configured step size, but don't exceed maximum */
                    new_size = ebuf->buffer_size + ebuf->config.growth_step_size;
                    if (new_size > ebuf->config.max_buffer_size) {
                        new_size = ebuf->config.max_buffer_size;
                    }
                    MB_LOG_INFO("Growing buffer: %zu -> %zu bytes (fill_ratio: %.2f, overflows: %d)",
                                ebuf->buffer_size, new_size, current_fill_ratio, ebuf->consecutive_overflows);
                } else if (should_shrink) {
                    /* Shrink by configured step size, but don't go below minimum */
                    new_size = ebuf->buffer_size - ebuf->config.shrink_step_size;
                    if (new_size < ebuf->config.min_buffer_size) {
                        new_size = ebuf->config.min_buffer_size;
                    }
                    MB_LOG_INFO("Shrinking buffer: %zu -> %zu bytes (fill_ratio: %.2f)",
                                ebuf->buffer_size, new_size, current_fill_ratio);
                }

                /* Apply resize if different */
                if (new_size != ebuf->buffer_size) {
                    int ret = l3_resize_buffer(ebuf, new_size);
                    if (ret == L3_SUCCESS) {
                        ebuf->last_resize_time = now;
                        /* Reset overflow/underflow counters after successful resize */
                        ebuf->consecutive_overflows = 0;
                        ebuf->consecutive_underflows = 0;
                    }
                }
            }
        }
    }

    return L3_SUCCESS;
}

int l3_get_buffer_metrics(l3_enhanced_double_buffer_t *ebuf, l3_buffer_metrics_t *metrics)
{
    if (!ebuf || !metrics) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&ebuf->mutex);

    /* Copy current metrics */
    *metrics = ebuf->metrics;

    /* Update current values */
    metrics->current_usage = ebuf->main_len + ebuf->sub_len;
    metrics->current_level = ebuf->current_watermark;

    pthread_mutex_unlock(&ebuf->mutex);

    return L3_SUCCESS;
}

/* ========== Memory Pool Management Functions ========== */

int l3_memory_pool_init(l3_memory_pool_t *pool, size_t pool_size, size_t block_size)
{
    if (!pool || pool_size == 0 || block_size == 0) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(pool, 0, sizeof(l3_memory_pool_t));

    /* Allocate memory pool */
    pool->pool_memory = malloc(pool_size);
    if (!pool->pool_memory) {
        MB_LOG_ERROR("Failed to allocate memory pool of %zu bytes", pool_size);
        return L3_ERROR_MEMORY;
    }

    /* Initialize pool parameters */
    pool->pool_size = pool_size;
    pool->block_size = block_size;
    pool->total_blocks = pool_size / block_size;
    pool->free_blocks = pool->total_blocks;

    /* Initialize mutex */
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize memory pool mutex");
        free(pool->pool_memory);
        return L3_ERROR_THREAD;
    }

    /* Initially all blocks are free */
    pool->fragmentation_ratio = 0.0;
    pool->largest_free_block = pool_size;

    MB_LOG_DEBUG("Memory pool initialized: %zu bytes, %zu blocks of %zu bytes",
                pool_size, pool->total_blocks, block_size);

    return L3_SUCCESS;
}

unsigned char *l3_memory_pool_alloc(l3_memory_pool_t *pool)
{
    if (!pool || !pool->pool_memory) {
        return NULL;
    }

    pthread_mutex_lock(&pool->pool_mutex);

    if (pool->free_blocks == 0) {
        MB_LOG_DEBUG("Memory pool exhausted - no free blocks");
        pthread_mutex_unlock(&pool->pool_mutex);
        return NULL;
    }

    /* Simple allocation - return next free block */
    /* In a real implementation, this would use a free list */
    unsigned char *block = pool->pool_memory +
                          (pool->allocated_blocks * pool->block_size);

    if (pool->allocated_blocks < pool->total_blocks) {
        pool->allocated_blocks++;
        pool->free_blocks--;
        pool->allocation_count++;

        /* Update fragmentation metrics */
        pool->fragmentation_ratio = (double)(pool->total_blocks - pool->free_blocks) / pool->total_blocks;
        pool->largest_free_block = pool->free_blocks * pool->block_size;
    } else {
        block = NULL;
    }

    pthread_mutex_unlock(&pool->pool_mutex);
    return block;
}

int l3_memory_pool_free(l3_memory_pool_t *pool, unsigned char *block)
{
    if (!pool || !block) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&pool->pool_mutex);

    /* Simple free - in a real implementation, this would add to free list */
    if (pool->allocated_blocks > 0) {
        pool->allocated_blocks--;
        pool->free_blocks++;
        pool->free_count++;

        /* Update fragmentation metrics */
        pool->fragmentation_ratio = (double)(pool->total_blocks - pool->free_blocks) / pool->total_blocks;
        pool->largest_free_block = pool->free_blocks * pool->block_size;
    }

    pthread_mutex_unlock(&pool->pool_mutex);
    return L3_SUCCESS;
}

void l3_memory_pool_cleanup(l3_memory_pool_t *pool)
{
    if (!pool) {
        return;
    }

    free(pool->pool_memory);
    pthread_mutex_destroy(&pool->pool_mutex);
    memset(pool, 0, sizeof(l3_memory_pool_t));

    MB_LOG_DEBUG("Memory pool cleaned up");
}

/* ========== Pipeline Buffer Management Functions ========== */

bool l3_should_apply_backpressure(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return false;
    }

    /* Check if output buffer is getting full (high watermark) */
    size_t available = l3_double_buffer_available(&pipeline->buffers);
    bool buffer_high = available > L3_HIGH_WATERMARK;

    /* Check if backpressure has been active too long */
    if (pipeline->backpressure_active) {
        time_t now = time(NULL);
        if (now - pipeline->backpressure_start > L3_BACKPRESSURE_TIMEOUT_MS / 1000) {
            MB_LOG_WARNING("Pipeline %s: Backpressure timeout, forcing release", pipeline->name);
            return false;
        }
    }

    return buffer_high;
}