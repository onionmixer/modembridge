/*
 * level3.h - Level 3 Pipeline Management for ModemBridge
 *
 * Level 3 manages dual data pipelines between Level 1 (Serial/Modem) and Level 2 (Telnet):
 * - Pipeline 1: Level 1 serial receive → Level 2 telnet server send
 * - Pipeline 2: Level 2 telnet server receive → Level 1 serial send
 *
 * Features:
 * - Half-duplex operation based on serial port limitations
 * - Double buffering (main/sub buffers) for each pipeline
 * - Fair scheduling to prevent starvation
 * - Backpressure mechanism with watermarks
 * - Protocol-specific filtering (Hayes codes, TELNET control codes)
 */

#ifndef MODEMBRIDGE_LEVEL3_H
#define MODEMBRIDGE_LEVEL3_H

#include "common.h"
#include "config.h"
#include "serial.h"
#include "modem.h"
#include "telnet.h"
#include "bridge.h"
#include "util.h"
#include "level3_types.h"  /* All type definitions for Level 3 */
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

/* Override buffer size to use UTIL constants if needed */
#ifdef UTIL_MAX_MESSAGE_LEN
#undef L3_PIPELINE_BUFFER_SIZE
#undef L3_MAX_BURST_SIZE
#define L3_PIPELINE_BUFFER_SIZE        UTIL_MAX_MESSAGE_LEN      /* Using common buffer size */
#define L3_MAX_BURST_SIZE             UTIL_MAX_MESSAGE_LEN       /* Using common message size */
#endif

/* Type definitions have been moved to level3_types.h */

/* Struct definitions that are too complex to move to level3_types.h remain here */

/* Hayes Command Entry */
typedef struct {
    const char *command;                 /* Command string (e.g., "ATE", "ATH") */
    hayes_command_type_t type;           /* Command type */
    bool has_parameter;                  /* Whether command accepts parameters */
    int min_param;                       /* Minimum parameter value */
    int max_param;                       /* Maximum parameter value */
    const char *description;             /* Human-readable description */
} hayes_command_entry_t;

/* Hayes Result Codes */
typedef struct {
    const char *code;                    /* Result code string */
    bool is_connection_result;           /* TRUE for CONNECT, NO CARRIER, etc. */
    bool ends_command_mode;              /* TRUE if switches to online mode */
} hayes_result_entry_t;

/* Hayes Command Dictionary */
typedef struct {
    const hayes_command_entry_t *commands;  /* Array of commands */
    size_t num_commands;                     /* Number of commands */
    const hayes_result_entry_t *results;    /* Array of result codes */
    size_t num_results;                      /* Number of result codes */
} hayes_dictionary_t;

/* Hayes Filter Context */
typedef struct {
    hayes_filter_state_t state;          /* Current filter state */
    unsigned char command_buffer[256];   /* Command accumulation buffer */
    size_t command_len;                  /* Current command length */
    unsigned char result_buffer[256];    /* Result code buffer */
    size_t result_len;                   /* Current result length */

    /* Line buffering for complete commands */
    unsigned char line_buffer[256];      /* Line accumulation buffer */
    size_t line_len;                     /* Current line length */
    long long line_start_time;           /* When line started */

    int plus_count;                      /* Count of '+' for escape detection */
    long long plus_start_time;           /* Start time of +++ sequence */
    long long last_char_time;            /* Time of last character */
    bool in_online_mode;                 /* TRUE when in data/online mode */
    const hayes_dictionary_t *dict;      /* Command dictionary reference */
} hayes_filter_context_t;

/* TELNET filter state is defined in level3_types.h */

/* Double Buffer Structure for Each Pipeline */
typedef struct {
    /* Main buffers - currently being processed */
    unsigned char main_data[L3_PIPELINE_BUFFER_SIZE];
    size_t main_len;
    size_t main_pos;                     /* Current read position */

    /* Sub buffers - accumulating new data during processing */
    unsigned char sub_data[L3_PIPELINE_BUFFER_SIZE];
    size_t sub_len;

    /* Buffer management */
    pthread_mutex_t mutex;              /* Protect buffer switching */
    bool main_active;                   /* True if main buffer is being processed */

    /* Flow control */
    size_t bytes_processed;             /* Total bytes processed */
    size_t bytes_dropped;               /* Bytes dropped due to overflow */
    time_t last_activity;               /* Timestamp of last data transfer */
} l3_double_buffer_t;

/* Level 3 Pipeline Context */
typedef struct {
    /* Pipeline identification */
    l3_pipeline_direction_t direction;
    char name[64];                      /* Human-readable name */

    /* Double buffering */
    l3_double_buffer_t buffers;

    /* Protocol filtering */
    union {
        hayes_filter_context_t hayes_ctx;     /* For Serial→Telnet pipeline */
        telnet_filter_state_t telnet_state;   /* For Telnet→Serial pipeline */
    } filter_state;

    /* Flow control */
    l3_pipeline_state_t state;         /* Current pipeline state */

    /* Fair scheduling */
    time_t last_timeslice_start;        /* When our current timeslice started */
    int timeslice_duration_ms;           /* Current timeslice allocation */
    size_t bytes_in_timeslice;          /* Bytes processed in current timeslice */

    /* Backpressure management */
    bool backpressure_active;           /* True if downstream is blocked */
    time_t backpressure_start;          /* When backpressure started */

    /* Statistics */
    uint64_t total_bytes_processed;
    uint64_t total_bytes_dropped;
    uint64_t pipeline_switches;         /* Number of main/sub buffer switches */
    double avg_processing_time_ms;      /* Average processing time */
    time_t last_activity;               /* Timestamp of last data transfer */

} l3_pipeline_t;

/* Enhanced Scheduling Configuration - LEVEL3_WORK_TODO.txt Compliant */
typedef struct {
    /* Time-based scheduling */
    int timeslice_ms_serial_to_telnet;          /* Time allocation for pipeline 1 */
    int timeslice_ms_telnet_to_serial;          /* Time allocation for pipeline 2 */
    int max_latency_ms;                         /* Maximum allowed latency per direction */

    /* Byte-based scheduling */
    size_t quantum_bytes;                       /* Bytes per scheduling quantum */
    size_t min_quantum_bytes;                   /* Minimum bytes to justify a slice */
    size_t max_quantum_bytes;                   /* Maximum bytes per slice */

    /* Fair scheduling */
    bool adaptive_scheduling;                   /* Enable adaptive timeslice adjustment */
    int max_consecutive_slices;                 /* Max consecutive slices per pipeline */
    int round_robin_weight_serial;              /* Weight for Serial→Telnet direction */
    int round_robin_weight_telnet;              /* Weight for Telnet→Serial direction */

    /* Low-speed optimization */
    bool low_speed_fairness;                   /* Enable 1200 bps fairness */
    int low_speed_boost_factor;                 /* Boost factor for low-speed direction */

    /* Anti-starvation */
    int starvation_timeout_ms;                  /* Time before forced slice to starving direction */
    size_t max_backlog_bytes;                   /* Maximum backlog before forced processing */

    /* Enhanced quantum enforcement (for compatibility with implementation) */
    int base_quantum_ms;                        /* Base quantum in milliseconds */
    int min_quantum_ms;                         /* Minimum quantum in milliseconds */
    int max_quantum_ms;                         /* Maximum quantum in milliseconds */
    float weight_balance_ratio;                 /* Weight balance ratio for fairness */
    int starvation_threshold_ms;                /* Starvation detection threshold */
    bool adaptive_quantum_enabled;              /* Enable adaptive quantum adjustment */
    bool fair_queue_enabled;                    /* Enable fair queue scheduling */

    /* Latency Bound Guarantee - LEVEL3_WORK_TODO.txt Compliant */
    int latency_bound_ms;                       /* Maximum latency bound per direction (ms) */
} l3_scheduling_config_t;

/* Fair Scheduling Statistics */
typedef struct {
    /* Time-based statistics */
    double avg_latency_ms[LEVEL3_DIRECTION_COUNT];
    int max_latency_samples[LEVEL3_DIRECTION_COUNT];
    int latency_exceedances[LEVEL3_DIRECTION_COUNT];

    /* Byte-based statistics */
    uint64_t bytes_processed[LEVEL3_DIRECTION_COUNT];
    uint64_t quantum_count[LEVEL3_DIRECTION_COUNT];
    size_t avg_quantum_size[LEVEL3_DIRECTION_COUNT];

    /* Fairness statistics */
    int consecutive_slices[LEVEL3_DIRECTION_COUNT];
    int forced_slices[LEVEL3_DIRECTION_COUNT];       /* Anti-starvation forced slices */
    int starvations_detected[LEVEL3_DIRECTION_COUNT];

    /* Performance metrics */
    double fairness_ratio;                      /* 0.0 = completely unfair, 1.0 = perfectly fair */
    double system_utilization;                  /* Overall system utilization */
    uint64_t total_scheduling_cycles;

    time_t last_update_time;                    /* When statistics were last updated */

} l3_scheduling_stats_t;

/* Latency Tracking per Direction */
typedef struct {
    time_t last_schedule_time;                  /* Last time this direction was scheduled */
    time_t last_process_start_time;             /* When last processing started */
    time_t last_process_end_time;               /* When last processing ended */

    /* Latency measurements */
    int current_latency_ms;                     /* Current measured latency */
    int avg_latency_ms;                         /* Moving average latency */
    int max_latency_ms;                         /* Maximum latency observed */
    int min_latency_ms;                         /* Minimum latency observed */

    /* Latency violation tracking */
    int latency_violations;                    /* Times latency exceeded limit */
    time_t last_violation_time;                /* Last time latency was violated */

    /* Backlog tracking */
    size_t current_backlog_bytes;               /* Current bytes waiting */
    size_t peak_backlog_bytes;                  /* Peak backlog observed */
    time_t peak_backlog_time;                   /* When peak occurred */

} l3_latency_tracker_t;

/* Level 3 Context Structure */
typedef struct {
    /* Bridge context reference */
    bridge_ctx_t *bridge;

    /* Enhanced State Machine - LEVEL3_WORK_TODO.txt Compliant */
    l3_system_state_t system_state;             /* Current system state */
    l3_system_state_t previous_state;           /* Previous system state */
    time_t state_change_time;                   /* When state changed */
    int state_timeout;                          /* Timeout for current state (seconds) */
    int state_transitions;                      /* Total state transitions */

    /* Pipeline management */
    l3_pipeline_t pipeline_serial_to_telnet;    /* Pipeline 1 */
    l3_pipeline_t pipeline_telnet_to_serial;    /* Pipeline 2 */

    /* Half-duplex control */
    l3_pipeline_direction_t active_pipeline;    /* Currently active pipeline */
    bool half_duplex_mode;                      /* True if half-duplex enabled */
    time_t last_pipeline_switch;                /* Last pipeline switch time */

    /* Enhanced Fair Scheduling */
    pthread_mutex_t scheduling_mutex;           /* Protect scheduling decisions */
    time_t scheduling_start_time;               /* When current round started */
    int round_robin_counter;                    /* Track fair scheduling */

    /* Scheduling configuration */
    l3_scheduling_config_t sched_config;        /* Scheduling parameters */
    l3_scheduling_stats_t sched_stats;          /* Scheduling statistics */

    /* Latency tracking */
    l3_latency_tracker_t latency_tracker[LEVEL3_DIRECTION_COUNT];  /* Per-direction latency */

    /* Fair queue management */
    int fair_queue_weights[LEVEL3_DIRECTION_COUNT];  /* Dynamic weights for fairness */
    time_t last_direction_switch[LEVEL3_DIRECTION_COUNT];  /* When each direction was last processed */
    size_t bytes_in_current_cycle[LEVEL3_DIRECTION_COUNT];  /* Bytes processed in current cycle */

    /* Enhanced scheduling state (for compatibility with implementation) */
    struct {
        l3_pipeline_direction_t current_direction;      /* Current active direction */
        long long last_direction_switch_time;          /* Last switch timestamp */
        int consecutive_slices;                        /* Consecutive slices in same direction */
        long long serial_starvation_time;              /* Serial starvation timestamp */
        long long telnet_starvation_time;              /* Telnet starvation timestamp */
    } sched_state;

    /* Quantum enforcement state */
    struct {
        int current_quantum_ms;                         /* Current quantum duration */
        long long start_time;                           /* Quantum start timestamp */
        size_t bytes_processed;                         /* Bytes processed in current quantum */
        size_t max_bytes_per_quantum;                   /* Maximum bytes per quantum */
    } quantum_state;

    /* Latency statistics aggregation */
    struct {
        double serial_to_telnet_avg_ms;                /* Average serial→telnet latency */
        double telnet_to_serial_avg_ms;                /* Average telnet→serial latency */
        double max_serial_to_telnet_ms;                /* Maximum serial→telnet latency */
        double max_telnet_to_serial_ms;                /* Maximum telnet→serial latency */
        uint64_t total_samples;                        /* Total latency samples */
        long long last_measurement_time;               /* Last measurement timestamp */
    } latency_stats;

    /* Fair queue state */
    struct {
        int serial_weight;                              /* Serial queue weight */
        int telnet_weight;                              /* Telnet queue weight */
        int serial_deficit;                             /* Serial deficit counter */
        int telnet_deficit;                             /* Telnet deficit counter */
    } fair_queue;

    /* System state */
    bool level3_active;                         /* True if Level 3 is enabled */
    bool level1_ready;                          /* Level 1 connection ready */
    bool level2_ready;                          /* Level 2 connection ready */
    bool dcd_rising_detected;                   /* DCD rising edge detected */
    bool negotiation_complete;                  /* Protocol negotiation complete */

    /* DCD State Management */
    bool dcd_state;                             /* Current DCD signal state */
    time_t dcd_change_time;                     /* Last DCD state change timestamp */

    /* Performance monitoring */
    uint64_t total_pipeline_switches;
    double system_utilization_pct;              /* Overall system utilization */
    time_t start_time;                          /* Level 3 activation time */
    long long system_start_time;                /* System start timestamp for latency calculations */

    /* System configuration for latency calculations */
    struct {
        int serial_baudrate;                     /* Current serial baudrate */
        bool low_speed_mode;                     /* True if <= 2400 bps */
    } system_config;

    /* Thread control */
    pthread_t level3_thread;                    /* Level 3 management thread */
    bool thread_running;                        /* Thread execution flag */
    bool shutdown_requested;                    /* Graceful shutdown requested */

    /* State Machine Control */
    pthread_mutex_t state_mutex;               /* Protect state transitions */
    pthread_cond_t state_condition;             /* Signal state changes */

} l3_context_t;

/* Include state machine functions after l3_context_t is defined */
#include "level3_state.h"

/* Enhanced Buffer Management - LEVEL3_WORK_TODO.txt Compliant */

/* Buffer Watermark Levels defined in level3_types.h */

/* Enhanced Buffer Metrics */
typedef struct {
    /* Current state */
    size_t current_usage;            /* Current bytes in use */
    size_t peak_usage;               /* Peak usage observed */
    size_t min_free_space;           /* Minimum free space observed */

    /* Watermark tracking */
    l3_watermark_level_t current_level;
    l3_watermark_level_t peak_level;
    time_t time_at_peak_level;

    /* Overflow and underflow statistics */
    uint64_t overflow_events;        /* Times buffer was full */
    uint64_t underflow_events;       /* Times buffer was empty when read */
    uint64_t bytes_dropped;          /* Total bytes dropped due to overflow */

    /* Memory fragmentation tracking */
    size_t fragmentation_count;      /* Number of fragmented blocks */
    size_t largest_fragment_size;    /* Size of largest contiguous block */

    /* Performance metrics */
    double avg_fill_ratio;           /* Average buffer fill ratio */
    time_t last_activity;            /* Last time data was written/read */

} l3_buffer_metrics_t;

/* Dynamic Buffer Configuration */
typedef struct {
    /* Size management */
    size_t min_buffer_size;          /* Minimum buffer size */
    size_t max_buffer_size;          /* Maximum buffer size */
    size_t current_buffer_size;      /* Current buffer size */

    /* Adaptive sizing */
    bool adaptive_sizing_enabled;    /* Enable dynamic buffer sizing */
    size_t growth_threshold;         /* Usage percentage to trigger growth */
    size_t shrink_threshold;         /* Usage percentage to trigger shrink */
    size_t growth_step_size;         /* Bytes to grow when expanding */
    size_t shrink_step_size;         /* Bytes to shrink when contracting */

    /* Watermark configuration */
    size_t critical_watermark;       /* Critical level (>95%) */
    size_t high_watermark;           /* High level (>80%) */
    size_t low_watermark;            /* Low level (<20%) */
    size_t empty_watermark;          /* Empty level (<5%) */

    /* Flow control */
    bool backpressure_enabled;       /* Enable backpressure mechanism */
    bool flow_control_enabled;       /* Enable hardware flow control */
    int flow_control_threshold;      /* Threshold for flow control activation */

} l3_buffer_config_t;

/* Memory Pool for Fragmentation Prevention */
typedef struct {
    /* Pool management */
    unsigned char *pool_memory;      /* Allocated memory pool */
    size_t pool_size;                /* Total pool size */
    size_t block_size;               /* Size of each block */
    size_t total_blocks;             /* Total number of blocks */

    /* Allocation tracking */
    size_t free_blocks;              /* Number of free blocks */
    size_t allocated_blocks;         /* Number of allocated blocks */
    uint64_t allocation_count;       /* Total allocations performed */
    uint64_t free_count;             /* Total frees performed */

    /* Fragmentation metrics */
    double fragmentation_ratio;      /* Current fragmentation ratio */
    size_t largest_free_block;       /* Size of largest contiguous free block */
    size_t free_list_length;         /* Number of free block entries */

    /* Synchronization */
    pthread_mutex_t pool_mutex;      /* Protect pool operations */

} l3_memory_pool_t;

/* Enhanced Double Buffer with Watermark Defense */
typedef struct {
    /* Basic buffer structure (existing) */
    unsigned char *main_data;        /* Main buffer - dynamically allocated */
    unsigned char *sub_data;         /* Sub buffer - dynamically allocated */
    size_t main_len;
    size_t main_pos;
    size_t sub_len;
    size_t buffer_size;              /* Current buffer size (dynamic) */

    /* Enhanced buffer management */
    l3_buffer_config_t config;       /* Buffer configuration */
    l3_buffer_metrics_t metrics;     /* Buffer performance metrics */
    l3_memory_pool_t memory_pool;    /* Memory pool for allocation */

    /* Watermark state management */
    l3_watermark_level_t current_watermark;
    time_t watermark_change_time;
    bool backpressure_active;

    /* Dynamic sizing state */
    time_t last_resize_time;
    int consecutive_overflows;       /* Track repeated overflows for growth */
    int consecutive_underflows;      /* Track repeated underflows for shrink */

    /* Buffer switching (existing) */
    pthread_mutex_t mutex;
    bool main_active;

    /* Flow control (existing + enhanced) */
    size_t bytes_processed;
    size_t bytes_dropped;
    time_t last_activity;

} l3_enhanced_double_buffer_t;

/* Internal Helper Functions - Static declarations in level3.c */

/* Function Prototypes */

/* ===== DCD Event Bridge Functions ===== */

/**
 * Initialize DCD monitoring for Level 3
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_init_dcd_monitoring(l3_context_t *l3_ctx);

/* State machine functions are now in level3_state.h */

/* Level 3 Context Management */
/**
 * Initialize Level 3 context
 * @param l3_ctx Level 3 context to initialize
 * @param bridge_ctx Bridge context (provides access to serial/telnet)
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_init(l3_context_t *l3_ctx, bridge_ctx_t *bridge_ctx);

/**
 * Start Level 3 pipeline management
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_start(l3_context_t *l3_ctx);

/**
 * Stop Level 3 pipeline management
 * @param l3_ctx Level 3 context
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_stop(l3_context_t *l3_ctx);

/**
 * Cleanup Level 3 resources
 * @param l3_ctx Level 3 context
 */
void l3_cleanup(l3_context_t *l3_ctx);

/* Pipeline Management */
/**
 * Initialize a specific pipeline
 * @param pipeline Pipeline structure to initialize
 * @param direction Pipeline direction (serial→telnet or telnet→serial)
 * @param name Human-readable pipeline name
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_pipeline_init(l3_pipeline_t *pipeline, l3_pipeline_direction_t direction, const char *name);

/**
 * Process data through a pipeline
 * @param pipeline Pipeline to process
 * @param input_data Input data buffer
 * @param input_len Input data length
 * @param output_data Output buffer for processed data
 * @param output_len Pointer to store output length
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_pipeline_process(l3_pipeline_t *pipeline, const unsigned char *input_data, size_t input_len,
                       unsigned char *output_data, size_t output_size, size_t *output_len);

/**
 * Switch main/sub buffers in a pipeline
 * @param pipeline Pipeline context
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_pipeline_switch_buffers(l3_pipeline_t *pipeline);

/* Double Buffer Management */
/**
 * Initialize double buffer structure
 * @param dbuf Double buffer to initialize
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_double_buffer_init(l3_double_buffer_t *dbuf);

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

/* Enhanced Buffer Management - LEVEL3_WORK_TODO.txt Compliant */

/**
 * Initialize enhanced double buffer with watermark defense
 * @param ebuf Enhanced double buffer to initialize
 * @param initial_size Initial buffer size
 * @param min_size Minimum buffer size
 * @param max_size Maximum buffer size
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_enhanced_double_buffer_init(l3_enhanced_double_buffer_t *ebuf,
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
 * Check if buffer needs dynamic resizing
 * @param ebuf Enhanced double buffer context
 * @param should_grow Pointer to store if buffer should grow
 * @param should_shrink Pointer to store if buffer should shrink
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_check_resize_needed(l3_enhanced_double_buffer_t *ebuf, bool *should_grow, bool *should_shrink);

/**
 * Resize enhanced buffer dynamically
 * @param ebuf Enhanced double buffer context
 * @param new_size New buffer size
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_resize_buffer(l3_enhanced_double_buffer_t *ebuf, size_t new_size);

/**
 * Update buffer metrics after operation
 * @param ebuf Enhanced double buffer context
 * @param bytes_written Number of bytes written (0 if read operation)
 * @param bytes_read Number of bytes read (0 if write operation)
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_update_buffer_metrics(l3_enhanced_double_buffer_t *ebuf,
                             size_t bytes_written, size_t bytes_read);

/**
 * Get comprehensive buffer statistics
 * @param ebuf Enhanced double buffer context
 * @param metrics Output structure for buffer metrics
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_get_buffer_metrics(l3_enhanced_double_buffer_t *ebuf, l3_buffer_metrics_t *metrics);

/**
 * Initialize memory pool for fragmentation prevention
 * @param pool Memory pool to initialize
 * @param pool_size Total size of memory pool
 * @param block_size Size of individual blocks
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_memory_pool_init(l3_memory_pool_t *pool, size_t pool_size, size_t block_size);

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
l3_result_t l3_memory_pool_free(l3_memory_pool_t *pool, unsigned char *block);

/**
 * Cleanup memory pool and release all memory
 * @param pool Memory pool to cleanup
 */
void l3_memory_pool_cleanup(l3_memory_pool_t *pool);

/**
 * Check and apply backpressure based on watermark level
 * @param ebuf Enhanced double buffer context
 * @return true if backpressure should be applied
 */
bool l3_should_apply_enhanced_backpressure(l3_enhanced_double_buffer_t *ebuf);

/**
 * Get watermark level name as string
 * @param level Watermark level
 * @return String representation of level
 */
const char *l3_watermark_level_to_string(l3_watermark_level_t level);

/* Protocol Filtering */
/**
 * Filter Hayes commands from serial data (Pipeline 1)
 * @param ctx Hayes filter context (maintained across calls)
 * @param input Input data from serial port
 * @param input_len Input data length
 * @param output Output buffer for filtered data
 * @param output_size Output buffer size
 * @param output_len Pointer to store output length
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_filter_hayes_commands(hayes_filter_context_t *ctx, const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_size, size_t *output_len);

/**
 * Filter TELNET control codes from telnet data (Pipeline 2)
 * @param state TELNET filter state (maintained across calls)
 * @param input Input data from telnet server
 * @param input_len Input data length
 * @param output Output buffer for filtered data
 * @param output_size Output buffer size
 * @param output_len Pointer to store output length
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_filter_telnet_controls(telnet_filter_state_t *state, const unsigned char *input, size_t input_len,
                             unsigned char *output, size_t output_size, size_t *output_len);

/* Scheduling and Fairness */
/**
 * Initialize fair scheduling
 * @param l3_ctx Level 3 context
 * @param config Scheduling configuration
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_scheduling_init(l3_context_t *l3_ctx, const l3_scheduling_config_t *config);

/**
 * Determine which pipeline should run next (fair scheduling)
 * @param l3_ctx Level 3 context
 * @return Pipeline direction that should run next
 */
l3_pipeline_direction_t l3_schedule_next_pipeline(l3_context_t *l3_ctx);

/**
 * Update scheduling statistics after pipeline execution
 * @param l3_ctx Level 3 context
 * @param pipeline Pipeline that just executed
 * @param bytes_processed Number of bytes processed
 * @param processing_time_ms Time taken in milliseconds
 */
void l3_update_scheduling_stats(l3_context_t *l3_ctx, l3_pipeline_t *pipeline,
                               size_t bytes_processed, double processing_time_ms);

/* Backpressure Management */
/**
 * Check if backpressure should be applied to a pipeline
 * @param pipeline Pipeline to check
 * @return true if backpressure should be applied
 */
bool l3_should_apply_backpressure(l3_pipeline_t *pipeline);

/**
 * Apply backpressure to a pipeline
 * @param pipeline Pipeline to apply backpressure to
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_apply_backpressure(l3_pipeline_t *pipeline);

/**
 * Release backpressure on a pipeline
 * @param pipeline Pipeline to release backpressure
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_release_backpressure(l3_pipeline_t *pipeline);

/* Half-duplex Control */
/**
 * Switch active pipeline (half-duplex operation)
 * @param l3_ctx Level 3 context
 * @param new_active_pipeline Pipeline to switch to
 * @return L3_SUCCESS on success, l3_result_t error code on failure
 */
l3_result_t l3_switch_active_pipeline(l3_context_t *l3_ctx, l3_pipeline_direction_t new_active_pipeline);

/**
 * Check if pipeline switching is allowed
 * @param l3_ctx Level 3 context
 * @return true if switching is allowed
 */
bool l3_can_switch_pipeline(l3_context_t *l3_ctx);

/* Statistics and Monitoring */
/**
 * Print Level 3 statistics
 * @param l3_ctx Level 3 context
 */
void l3_print_stats(l3_context_t *l3_ctx);

/**
 * Print pipeline-specific statistics
 * @param pipeline Pipeline context
 */
void l3_print_pipeline_stats(l3_pipeline_t *pipeline);

/**
 * Get system utilization percentage
 * @param l3_ctx Level 3 context
 * @return Utilization percentage (0.0 to 100.0)
 */
double l3_get_system_utilization(l3_context_t *l3_ctx);

/* Thread Functions */
/**
 * Level 3 management thread function
 * Handles pipeline switching, scheduling, and monitoring
 * @param arg Pointer to l3_context_t
 * @return NULL
 */
void *l3_management_thread_func(void *arg);

/* Utility Functions */
/**
 * Get pipeline name from direction
 * @param direction Pipeline direction
 * @return Human-readable pipeline name
 */
const char *l3_get_pipeline_name(l3_pipeline_direction_t direction);

/**
 * Get current timestamp in milliseconds
 * @return Timestamp in milliseconds
 */
long long l3_get_timestamp_ms(void);

/**
 * Convert pipeline state to string
 * @param state Pipeline state
 * @return String representation of state
 */
const char *l3_pipeline_state_to_string(l3_pipeline_state_t state);

#endif /* MODEMBRIDGE_LEVEL3_H */