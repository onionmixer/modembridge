/*
 * level3.c - Level 3 Pipeline Management Implementation for ModemBridge
 *
 * This module implements the dual pipeline system that manages data flow between
 * Level 1 (Serial/Modem) and Level 2 (Telnet) with half-duplex operation,
 * fair scheduling, and protocol-specific filtering.
 */

#include "level3.h"
#include "level3_util.h"    /* Utility functions */
#include "level3_buffer.h"  /* Buffer management functions */
#include "level3_state.h"   /* State machine functions */
#include "level3_schedule.h" /* Scheduling and fairness functions */
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>


/* Forward declaration of Hayes dictionary */
static const hayes_dictionary_t hayes_dictionary;

/* Internal helper functions - Forward declarations */
static void l3_update_pipeline_stats(l3_pipeline_t *pipeline, size_t bytes_processed, double processing_time);
/* Scheduling functions moved to level3_schedule.c */
/* l3_get_direction_name is now in level3_util.c as public function */
static int l3_process_serial_to_telnet_chunk(l3_context_t *l3_ctx);
static int l3_process_telnet_to_serial_chunk(l3_context_t *l3_ctx);

/* ========== Multibyte Character Handling ========== */

/**
 * Detect if byte is the start of a multibyte sequence
 * @param c Byte to check
 * @return true if multibyte start, false otherwise
 */
static bool l3_is_multibyte_start(unsigned char c)
{
    /* UTF-8 multibyte start */
    if ((c & 0xE0) == 0xC0) return true;  /* 2-byte UTF-8 */
    if ((c & 0xF0) == 0xE0) return true;  /* 3-byte UTF-8 */
    if ((c & 0xF8) == 0xF0) return true;  /* 4-byte UTF-8 */

    /* EUC-KR/EUC-JP high byte (0xA1-0xFE) */
    if (c >= 0xA1 && c <= 0xFE) return true;

    /* SHIFT-JIS first byte */
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) return true;

    return false;
}

/**
 * Get expected multibyte sequence length
 * @param c First byte of sequence
 * @return Expected total length (including first byte)
 */
static int l3_get_multibyte_length(unsigned char c)
{
    /* UTF-8 */
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;

    /* EUC-KR/EUC-JP: 2 bytes for hangul/kanji */
    if (c >= 0xA1 && c <= 0xFE) return 2;

    /* SHIFT-JIS: usually 2 bytes */
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) return 2;

    return 1;  /* Single byte */
}

/**
 * Check if multibyte sequence is complete
 * @param buffer Buffer containing bytes
 * @param len Current buffer length
 * @param expected Expected total length
 * @return true if complete, false otherwise
 */
static bool l3_is_multibyte_complete(const unsigned char *buffer, size_t len, int expected)
{
    if (len < (size_t)expected) return false;

    /* Additional validation for UTF-8 continuation bytes */
    unsigned char first = buffer[0];
    if ((first & 0xE0) == 0xC0 || (first & 0xF0) == 0xE0 || (first & 0xF8) == 0xF0) {
        for (size_t i = 1; i < len && i < (size_t)expected; i++) {
            if ((buffer[i] & 0xC0) != 0x80) {
                return false;  /* Invalid UTF-8 continuation */
            }
        }
    }

    return (len >= (size_t)expected);
}

/**
 * Send echo to modem via telnet-to-serial buffer
 * @param l3_ctx Level 3 context
 * @param data Data to echo
 * @param len Length of data
 * @return Number of bytes echoed
 */
static size_t l3_echo_to_modem(l3_context_t *l3_ctx, const unsigned char *data, size_t len)
{
    if (!l3_ctx || !data || len == 0) return 0;

    /* Write to telnet-to-serial buffer for echo */
    size_t written = ts_cbuf_write(&l3_ctx->bridge->ts_telnet_to_serial_buf, data, len);

    if (written > 0) {
        MB_LOG_DEBUG("Echo to modem: %zu bytes", written);

        /* Debug output for echo content */
        if (written <= 10) {
            char hex_str[32];
            size_t pos = 0;
            for (size_t i = 0; i < written && pos < sizeof(hex_str)-3; i++) {
                pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X ", data[i]);
            }
            MB_LOG_DEBUG("Echo bytes: %s", hex_str);
        }
    }

    return written;
}
/**
 * Initialize DCD monitoring for Level 3
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
int l3_init_dcd_monitoring(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&l3_ctx->state_mutex);

    /* Initialize DCD state */
    l3_ctx->dcd_state = false;
    l3_ctx->dcd_rising_detected = false;
    l3_ctx->dcd_change_time = time(NULL);

    MB_LOG_INFO("DCD monitoring initialized for Level 3");

    pthread_mutex_unlock(&l3_ctx->state_mutex);
    return L3_SUCCESS;
}
/* ========== Level 3 Context Management ========== */

int l3_init(l3_context_t *l3_ctx, bridge_ctx_t *bridge_ctx)
{
    if (l3_ctx == NULL || bridge_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(l3_ctx, 0, sizeof(l3_context_t));
    l3_ctx->bridge = bridge_ctx;

    /* Initialize state machine */
    l3_ctx->system_state = L3_STATE_UNINITIALIZED;
    l3_ctx->previous_state = L3_STATE_UNINITIALIZED;
    l3_ctx->state_change_time = time(NULL);
    l3_ctx->state_timeout = 0;
    l3_ctx->state_transitions = 0;

    /* Initialize state machine synchronization */
    if (pthread_mutex_init(&l3_ctx->state_mutex, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize state mutex");
        return L3_ERROR_THREAD;
    }
    if (pthread_cond_init(&l3_ctx->state_condition, NULL) != 0) {
        MB_LOG_ERROR("Failed to initialize state condition");
        pthread_mutex_destroy(&l3_ctx->state_mutex);
        return L3_ERROR_THREAD;
    }

    /* Set initial state */
    int ret = l3_set_system_state(l3_ctx, L3_STATE_INITIALIZING, LEVEL3_INIT_TIMEOUT);
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to set initial state");
        pthread_mutex_destroy(&l3_ctx->state_mutex);
        pthread_cond_destroy(&l3_ctx->state_condition);
        return ret;
    }

    /* Initialize pipelines */
    ret = l3_pipeline_init(&l3_ctx->pipeline_serial_to_telnet,
                           L3_PIPELINE_SERIAL_TO_TELNET,
                           "Serial→Telnet");
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to initialize Serial→Telnet pipeline");
        return ret;
    }

    ret = l3_pipeline_init(&l3_ctx->pipeline_telnet_to_serial,
                          L3_PIPELINE_TELNET_TO_SERIAL,
                          "Telnet→Serial");
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to initialize Telnet→Serial pipeline");
        return ret;
    }

    /* Initialize scheduling */
    pthread_mutex_init(&l3_ctx->scheduling_mutex, NULL);
    l3_ctx->scheduling_start_time = time(NULL);
    l3_ctx->round_robin_counter = 0;

    /* Initialize enhanced scheduling system */
    ret = l3_init_enhanced_scheduling(l3_ctx);
    if (ret != L3_SUCCESS) {
        MB_LOG_ERROR("Failed to initialize enhanced scheduling system");
        /* Continue with basic scheduling - not fatal */
    }

    /* Initialize half-duplex control */
    l3_ctx->active_pipeline = L3_PIPELINE_SERIAL_TO_TELNET;  /* Start with pipeline 1 */
    l3_ctx->half_duplex_mode = true;  /* Enable half-duplex by default */
    l3_ctx->last_pipeline_switch = time(NULL);

    /* Initialize system state */
    l3_ctx->level3_active = false;
    l3_ctx->level1_ready = false;
    l3_ctx->level2_ready = false;

    /* Initialize performance monitoring */
    l3_ctx->total_pipeline_switches = 0;
    l3_ctx->system_utilization_pct = 0.0;
    l3_ctx->start_time = time(NULL);

    /* Thread control */
    l3_ctx->thread_running = false;

    MB_LOG_INFO("Level 3 context initialized successfully");
    return L3_SUCCESS;
}

int l3_start(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    MB_LOG_INFO("Starting Level 3 pipeline management");

    /* Check if both Level 1 and Level 2 are ready */
    l3_ctx->level1_ready = l3_ctx->bridge->serial_ready && l3_ctx->bridge->modem_ready;

#ifdef ENABLE_LEVEL2
    l3_ctx->level2_ready = telnet_is_connected(&l3_ctx->bridge->telnet);
#else
    l3_ctx->level2_ready = false;
    MB_LOG_WARNING("Level 3: Level 2 (telnet) not available in this build");
#endif

    if (!l3_ctx->level1_ready) {
        MB_LOG_WARNING("Level 3: Level 1 (serial/modem) not ready - will wait");
    }

    if (!l3_ctx->level2_ready) {
        MB_LOG_WARNING("Level 3: Level 2 (telnet) not ready - will wait");
    }

    /* Start Level 3 management thread */
    l3_ctx->thread_running = true;
    l3_ctx->level3_active = true;

    int ret = pthread_create(&l3_ctx->level3_thread, NULL, l3_management_thread_func, l3_ctx);
    if (ret != 0) {
        MB_LOG_ERROR("Failed to create Level 3 management thread: %s", strerror(ret));
        l3_ctx->thread_running = false;
        l3_ctx->level3_active = false;
        return L3_ERROR_THREAD;
    }

    MB_LOG_INFO("Level 3 management thread started successfully");
    return L3_SUCCESS;
}

int l3_stop(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    MB_LOG_INFO("Stopping Level 3 pipeline management");

    /* Signal thread to stop */
    l3_ctx->thread_running = false;
    l3_ctx->level3_active = false;

    /* Wait for thread to exit */
    if (pthread_join(l3_ctx->level3_thread, NULL) != 0) {
        MB_LOG_WARNING("Failed to join Level 3 management thread");
    }

    /* Print final statistics */
    l3_print_stats(l3_ctx);

    MB_LOG_INFO("Level 3 pipeline management stopped");
    return L3_SUCCESS;
}

void l3_cleanup(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return;
    }

    /* Cleanup pipelines */
    pthread_mutex_destroy(&l3_ctx->pipeline_serial_to_telnet.buffers.mutex);
    pthread_mutex_destroy(&l3_ctx->pipeline_telnet_to_serial.buffers.mutex);

    /* Cleanup scheduling */
    pthread_mutex_destroy(&l3_ctx->scheduling_mutex);

    memset(l3_ctx, 0, sizeof(l3_context_t));
    MB_LOG_INFO("Level 3 context cleaned up");
}

/* ========== Pipeline Management ========== */

int l3_pipeline_init(l3_pipeline_t *pipeline, l3_pipeline_direction_t direction, const char *name)
{
    if (pipeline == NULL || name == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    memset(pipeline, 0, sizeof(l3_pipeline_t));
    pipeline->direction = direction;
    strncpy(pipeline->name, name, sizeof(pipeline->name) - 1);
    pipeline->name[sizeof(pipeline->name) - 1] = '\0';

    /* Initialize double buffer */
    int ret = l3_double_buffer_init(&pipeline->buffers);
    if (ret != L3_SUCCESS) {
        return ret;
    }

    /* Initialize protocol filter state */
    if (direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        /* Initialize Hayes filter context with dictionary */
        memset(&pipeline->filter_state.hayes_ctx, 0, sizeof(hayes_filter_context_t));
        pipeline->filter_state.hayes_ctx.state = HAYES_STATE_NORMAL;
        pipeline->filter_state.hayes_ctx.dict = &hayes_dictionary;
        pipeline->filter_state.hayes_ctx.in_online_mode = false;  /* Start in command mode */
        pipeline->filter_state.hayes_ctx.last_char_time = l3_get_timestamp_ms();
    } else {
        pipeline->filter_state.telnet_state = TELNET_FILTER_STATE_DATA;
    }

    /* Initialize fair scheduling */
    pipeline->last_timeslice_start = time(NULL);
    pipeline->timeslice_duration_ms = L3_FAIRNESS_TIME_SLICE_MS;
    pipeline->bytes_in_timeslice = 0;

    /* Initialize backpressure */
    pipeline->backpressure_active = false;
    pipeline->backpressure_start = 0;

    /* Initialize statistics */
    pipeline->total_bytes_processed = 0;
    pipeline->total_bytes_dropped = 0;
    pipeline->pipeline_switches = 0;
    pipeline->avg_processing_time_ms = 0.0;

    MB_LOG_DEBUG("Pipeline initialized: %s", pipeline->name);
    return L3_SUCCESS;
}

int l3_pipeline_process(l3_pipeline_t *pipeline, const unsigned char *input_data, size_t input_len,
                       unsigned char *output_data, size_t output_size, size_t *output_len)
{
    if (pipeline == NULL || input_data == NULL || output_data == NULL || output_len == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (input_len == 0) {
        *output_len = 0;
        return L3_SUCCESS;
    }

    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    size_t filtered_len = 0;
    int ret = L3_SUCCESS;

    /* Apply protocol-specific filtering */
    if (pipeline->direction == L3_PIPELINE_SERIAL_TO_TELNET) {
        /* Pipeline 1: Filter Hayes commands with enhanced dictionary support */
        ret = l3_filter_hayes_commands(&pipeline->filter_state.hayes_ctx,
                                       input_data, input_len,
                                       output_data, output_size, &filtered_len);
    } else {
        /* Pipeline 2: Filter TELNET control codes */
        ret = l3_filter_telnet_controls(&pipeline->filter_state.telnet_state,
                                        input_data, input_len,
                                        output_data, output_size, &filtered_len);
    }

    if (ret != L3_SUCCESS) {
        MB_LOG_WARNING("Pipeline %s: Filtering failed", pipeline->name);
        *output_len = 0;
        return ret;
    }

    /* Update pipeline statistics */
    gettimeofday(&end_time, NULL);
    double processing_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                            (end_time.tv_usec - start_time.tv_usec) / 1000.0;

    l3_update_pipeline_stats(pipeline, filtered_len, processing_time);

    *output_len = filtered_len;
    return L3_SUCCESS;
}

int l3_pipeline_switch_buffers(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&pipeline->buffers.mutex);

    /* Switch active buffers */
    pipeline->buffers.main_active = !pipeline->buffers.main_active;

    /* Reset the new main buffer's read position */
    if (!pipeline->buffers.main_active) {
        pipeline->buffers.main_pos = 0;
    } else {
        pipeline->buffers.main_pos = 0;
    }

    /* Clear the new sub buffer */
    if (pipeline->buffers.main_active) {
        pipeline->buffers.sub_len = 0;
    } else {
        pipeline->buffers.sub_len = 0;
    }

    pipeline->pipeline_switches++;

    pthread_mutex_unlock(&pipeline->buffers.mutex);

    MB_LOG_DEBUG("Pipeline %s: Switched buffers (switch #%llu)",
                pipeline->name, (unsigned long long)pipeline->pipeline_switches);
    return L3_SUCCESS;
}

/* ========== Double Buffer Management ========== */

/* ========== Protocol Filtering ========== */

/* Hayes Command Dictionary - Based on modem.c implementation */
static const hayes_command_entry_t hayes_commands[] = {
    /* Basic AT Commands */
    {"ATA",  HAYES_CMD_BASIC, false, 0, 0, "Answer incoming call"},
    {"ATB",  HAYES_CMD_BASIC, true,  0, 1, "Bell/CCITT mode"},
    {"ATD",  HAYES_CMD_BASIC, true,  0, 0, "Dial number"},
    {"ATE",  HAYES_CMD_BASIC, true,  0, 1, "Echo on/off"},
    {"ATH",  HAYES_CMD_BASIC, true,  0, 1, "Hang up"},
    {"ATI",  HAYES_CMD_BASIC, true,  0, 9, "Information/identification"},
    {"ATL",  HAYES_CMD_BASIC, true,  0, 3, "Speaker volume"},
    {"ATM",  HAYES_CMD_BASIC, true,  0, 3, "Speaker control"},
    {"ATO",  HAYES_CMD_BASIC, true,  0, 0, "Return to online mode"},
    {"ATQ",  HAYES_CMD_BASIC, true,  0, 1, "Quiet mode"},
    {"ATS",  HAYES_CMD_REGISTER, true, 0, 255, "S-register access"},
    {"ATV",  HAYES_CMD_BASIC, true,  0, 1, "Verbose mode"},
    {"ATX",  HAYES_CMD_BASIC, true,  0, 4, "Extended result codes"},
    {"ATZ",  HAYES_CMD_BASIC, true,  0, 1, "Reset modem"},

    /* Extended AT& Commands */
    {"AT&C", HAYES_CMD_EXTENDED, true, 0, 1, "DCD control"},
    {"AT&D", HAYES_CMD_EXTENDED, true, 0, 3, "DTR control"},
    {"AT&F", HAYES_CMD_EXTENDED, false, 0, 0, "Factory defaults"},
    {"AT&V", HAYES_CMD_EXTENDED, false, 0, 0, "View configuration"},
    {"AT&W", HAYES_CMD_EXTENDED, true, 0, 1, "Write configuration"},
    {"AT&S", HAYES_CMD_EXTENDED, true, 0, 1, "DSR override"},

    /* Escape sequence */
    {"+++",  HAYES_CMD_BASIC, false, 0, 0, "Escape to command mode"}
};

/* Hayes Result Codes */
static const hayes_result_entry_t hayes_results[] = {
    {"OK",           false, false},
    {"ERROR",        false, false},
    {"CONNECT",      true,  true},
    {"NO CARRIER",   true,  false},
    {"NO DIALTONE",  true,  false},
    {"BUSY",         true,  false},
    {"NO ANSWER",    true,  false},
    {"RING",         false, false},
    {"DELAYED",      false, false},
    {"BLACKLISTED",  false, false}
};

/* Global Hayes Dictionary */
static const hayes_dictionary_t hayes_dictionary = {
    .commands = hayes_commands,
    .num_commands = sizeof(hayes_commands) / sizeof(hayes_commands[0]),
    .results = hayes_results,
    .num_results = sizeof(hayes_results) / sizeof(hayes_results[0])
};

/**
 * Check if buffer contains a Hayes command
 */
static bool l3_is_hayes_command(const unsigned char *buffer, size_t len, const hayes_dictionary_t *dict)
{
    if (len < 2 || dict == NULL) return false;

    /* Check for "AT" prefix (case insensitive) */
    if ((buffer[0] == 'A' || buffer[0] == 'a') &&
        (buffer[1] == 'T' || buffer[1] == 't')) {

        /* For each known command */
        for (size_t i = 0; i < dict->num_commands; i++) {
            const char *cmd = dict->commands[i].command;
            size_t cmd_len = strlen(cmd);

            /* Check if buffer starts with this command (case insensitive) */
            if (len >= cmd_len) {
                bool match = true;
                for (size_t j = 0; j < cmd_len; j++) {
                    char b = buffer[j];
                    char c = cmd[j];
                    /* Case insensitive comparison */
                    if (b >= 'a' && b <= 'z') b -= 32;
                    if (c >= 'a' && c <= 'z') c -= 32;
                    if (b != c) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    MB_LOG_DEBUG("Detected Hayes command: %s", cmd);
                    return true;
                }
            }
        }
    }

    /* Check for +++ escape sequence */
    if (len >= 3 && buffer[0] == '+' && buffer[1] == '+' && buffer[2] == '+') {
        MB_LOG_DEBUG("Detected Hayes escape sequence: +++");
        return true;
    }

    return false;
}

/**
 * Check if buffer contains a Hayes result code
 */
static bool l3_is_hayes_result(const unsigned char *buffer, size_t len, const hayes_dictionary_t *dict)
{
    if (len < 2 || dict == NULL) return false;

    /* For each known result code */
    for (size_t i = 0; i < dict->num_results; i++) {
        const char *result = dict->results[i].code;
        size_t result_len = strlen(result);

        /* Check if buffer contains this result code */
        if (len >= result_len) {
            if (memcmp(buffer, result, result_len) == 0) {
                MB_LOG_DEBUG("Detected Hayes result: %s", result);
                return true;
            }
        }
    }

    return false;
}

/**
 * Enhanced Hayes filter with dictionary support and line buffering
 */
int l3_filter_hayes_commands(hayes_filter_context_t *ctx, const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t output_size, size_t *output_len)
{
    if (ctx == NULL || input == NULL || output == NULL || output_len == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Initialize dictionary if not set */
    if (ctx->dict == NULL) {
        ctx->dict = &hayes_dictionary;
    }

    size_t out_pos = 0;
    *output_len = 0;
    long long current_time = l3_get_timestamp_ms();

    /* Process input data */
    for (size_t i = 0; i < input_len && out_pos < output_size; i++) {
        unsigned char c = input[i];

        /* ONLINE MODE: Line buffering with AT command filtering */
        if (ctx->in_online_mode) {
            /* Special case: +++ escape sequence detection */
            if (c == '+') {
                /* Check for +++ escape sequence with guard time */
                if (ctx->plus_count == 0) {
                    long long elapsed = current_time - ctx->last_char_time;
                    if (elapsed >= 1000) { /* 1 second guard time before first + */
                        ctx->plus_start_time = current_time;
                        ctx->plus_count = 1;
                        ctx->last_char_time = current_time;
                        continue; /* Don't output yet */
                    }
                } else if (ctx->plus_count == 1 || ctx->plus_count == 2) {
                    ctx->plus_count++;
                    if (ctx->plus_count == 3) {
                        /* +++ detected - switch to command mode */
                        MB_LOG_INFO("Hayes filter: +++ escape detected, switching to COMMAND mode");
                        ctx->in_online_mode = false;
                        ctx->state = HAYES_STATE_NORMAL;
                        ctx->plus_count = 0;
                        ctx->line_len = 0;
                        continue; /* Don't output the +++ */
                    }
                    ctx->last_char_time = current_time;
                    continue; /* Don't output yet */
                }
            } else if (ctx->plus_count > 0) {
                /* Not a + - flush buffered + characters */
                for (int j = 0; j < ctx->plus_count; j++) {
                    if (out_pos < output_size) output[out_pos++] = '+';
                }
                ctx->plus_count = 0;
            }

            /* Line buffering for AT command detection */
            /* Track start time for new line */
            if (ctx->line_len == 0) {
                ctx->line_start_time = current_time;
            }

            /* Add to line buffer */
            if (ctx->line_len < sizeof(ctx->line_buffer) - 1) {
                ctx->line_buffer[ctx->line_len++] = c;
                ctx->line_buffer[ctx->line_len] = '\0';
            }

            /* Check if we have a complete line */
            if (c == '\r' || c == '\n') {
                /* Process complete line */
                bool is_at_command = false;

                /* Check for AT command (minimum 3 chars: "AT\r") */
                if (ctx->line_len >= 3) {
                    /* Check if line starts with AT (all 4 combinations: AT, At, aT, at) */
                    if ((ctx->line_buffer[0] == 'A' || ctx->line_buffer[0] == 'a') &&
                        (ctx->line_buffer[1] == 'T' || ctx->line_buffer[1] == 't')) {
                        /* Check what comes after AT - must be a command character or line ending */
                        if (ctx->line_len == 3) {
                            /* Just "AT\r" or "AT\n" */
                            is_at_command = true;
                        } else {
                            unsigned char next_char = ctx->line_buffer[2];
                            /* AT commands can be followed by:
                             * - Uppercase letter (ATH, ATZ, ATE, etc.)
                             * - Digit (AT0, AT1, etc.)
                             * - Special chars (+, &, %, etc. for extended commands)
                             * - CR/LF
                             */
                            if ((next_char >= 'A' && next_char <= 'Z') ||
                                (next_char >= '0' && next_char <= '9') ||
                                next_char == '+' || next_char == '&' ||
                                next_char == '%' || next_char == '\\' ||
                                next_char == '*' || next_char == '#' ||
                                next_char == '\r' || next_char == '\n') {
                                is_at_command = true;
                            }
                        }

                        if (is_at_command) {
                            MB_LOG_WARNING("Hayes filter: AT command BLOCKED in ONLINE mode: %.30s", ctx->line_buffer);
                        }
                    }
                }

                if (!is_at_command) {
                    /* Not an AT command - output the entire line */
                    for (size_t j = 0; j < ctx->line_len; j++) {
                        if (out_pos < output_size) {
                            output[out_pos++] = ctx->line_buffer[j];
                        }
                    }
                }
                /* Clear line buffer for next line */
                ctx->line_len = 0;
                memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
            } else {
                /* Not end of line - check for buffer overflow */
                if (ctx->line_len >= sizeof(ctx->line_buffer) - 1) {
                    /* Buffer full - flush it as it's probably not an AT command */
                    for (size_t j = 0; j < ctx->line_len; j++) {
                        if (out_pos < output_size) {
                            output[out_pos++] = ctx->line_buffer[j];
                        }
                    }
                    ctx->line_len = 0;
                    /* Add current character */
                    ctx->line_buffer[0] = c;
                    ctx->line_len = 1;
                }
            }

            ctx->last_char_time = current_time;
            continue;
        }

        /* COMMAND MODE - Original state machine for command processing */
        switch (ctx->state) {
            case HAYES_STATE_NORMAL:
                /* Command mode - accumulate line for AT command detection */
                /* Add to line buffer */
                size_t space_left = sizeof(ctx->line_buffer) - ctx->line_len - 1;
                if (space_left > 0) {
                    ctx->line_buffer[ctx->line_len++] = c;
                    ctx->line_buffer[ctx->line_len] = '\0';

                    /* Check for line ending */
                    bool has_line_ending = (c == '\r' || c == '\n');

                    if (has_line_ending) {
                        /* Process complete line */
                        if (ctx->line_len >= 3) { /* At least "AT" + line ending */
                            /* Check for AT command */
                            if ((ctx->line_buffer[0] == 'A' || ctx->line_buffer[0] == 'a') &&
                                (ctx->line_buffer[1] == 'T' || ctx->line_buffer[1] == 't')) {
                                /* AT command detected - process it */
                                /* Copy to command buffer for state machine */
                                memcpy(ctx->command_buffer, ctx->line_buffer, ctx->line_len);
                                ctx->command_len = ctx->line_len - 1; /* Exclude line ending */
                                ctx->state = HAYES_STATE_COMMAND;
                                MB_LOG_DEBUG("Hayes filter: AT command detected in line: %.20s", ctx->line_buffer);

                                /* Check if it's a known command */
                                if (l3_is_hayes_command(ctx->command_buffer, ctx->command_len, ctx->dict)) {
                                    /* Known command - filter it, wait for result */
                                    ctx->state = HAYES_STATE_CR_WAIT;
                                    MB_LOG_DEBUG("Hayes filter: Known command, waiting for result");
                                } else {
                                    /* Unknown command - pass through */
                                    for (size_t j = 0; j < ctx->line_len; j++) {
                                        if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                                    }
                                    ctx->state = HAYES_STATE_NORMAL;
                                }
                                ctx->command_len = 0;
                            } else {
                                /* Not an AT command - pass through */
                                for (size_t j = 0; j < ctx->line_len; j++) {
                                    if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                                }
                            }
                        } else {
                            /* Line too short for AT command - pass through */
                            for (size_t j = 0; j < ctx->line_len; j++) {
                                if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                            }
                        }
                        /* Clear line buffer */
                        ctx->line_len = 0;
                        memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
                    }
                    /* Otherwise continue buffering until line ending */
                } else {
                    /* Buffer overflow - flush and reset */
                    for (size_t j = 0; j < ctx->line_len; j++) {
                        if (out_pos < output_size) output[out_pos++] = ctx->line_buffer[j];
                    }
                    if (out_pos < output_size) output[out_pos++] = c;
                    ctx->line_len = 0;
                    memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
                }
                break;

            case HAYES_STATE_CR_WAIT:
                /* Waiting for CR after command */
                if (c == '\r') {
                    ctx->state = HAYES_STATE_LF_WAIT;
                } else if (c == '\n') {
                    ctx->state = HAYES_STATE_RESULT;
                    ctx->result_len = 0;
                } else {
                    /* Unexpected - back to normal */
                    output[out_pos++] = c;
                    ctx->state = HAYES_STATE_NORMAL;
                }
                break;

            case HAYES_STATE_LF_WAIT:
                /* Waiting for LF after CR */
                if (c == '\n') {
                    ctx->state = HAYES_STATE_RESULT;
                    ctx->result_len = 0;
                } else {
                    /* Unexpected - back to normal */
                    output[out_pos++] = c;
                    ctx->state = HAYES_STATE_NORMAL;
                }
                break;

            case HAYES_STATE_RESULT:
                /* Accumulate result code */
                if (c == '\r' || c == '\n') {
                    /* Check if it's a known result */
                    if (l3_is_hayes_result(ctx->result_buffer, ctx->result_len, ctx->dict)) {
                        /* Check if this result switches to online mode */
                        for (size_t j = 0; j < ctx->dict->num_results; j++) {
                            if (memcmp(ctx->result_buffer, ctx->dict->results[j].code,
                                      strlen(ctx->dict->results[j].code)) == 0) {
                                if (ctx->dict->results[j].ends_command_mode) {
                                    ctx->in_online_mode = true;
                                    MB_LOG_INFO("Hayes filter: CONNECT detected, switching to ONLINE mode");
                                }
                                break;
                            }
                        }
                    } else {
                        /* Unknown result - pass through */
                        for (size_t j = 0; j < ctx->result_len; j++) {
                            output[out_pos++] = ctx->result_buffer[j];
                        }
                        output[out_pos++] = c;
                    }
                    ctx->state = HAYES_STATE_NORMAL;
                    ctx->result_len = 0;
                } else if (ctx->result_len < sizeof(ctx->result_buffer) - 1) {
                    ctx->result_buffer[ctx->result_len++] = c;
                }
                /* Don't output result characters */
                break;

            default:
                /* Unexpected state - reset */
                ctx->state = HAYES_STATE_NORMAL;
                output[out_pos++] = c;
                break;
        }

        ctx->last_char_time = current_time;
    }

    /* Note: No need to flush in ONLINE mode since we output immediately */
    /* The line buffer is only used for AT command detection, not for output buffering */

    *output_len = out_pos;
    return L3_SUCCESS;
}

int l3_filter_telnet_controls(telnet_filter_state_t *state, const unsigned char *input, size_t input_len,
                             unsigned char *output, size_t output_size, size_t *output_len)
{
    if (state == NULL || input == NULL || output == NULL || output_len == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    size_t out_pos = 0;
    telnet_filter_state_t current_state = *state;
    *output_len = 0;

    for (size_t i = 0; i < input_len && out_pos < output_size; i++) {
        unsigned char c = input[i];

        switch (current_state) {
            case TELNET_FILTER_STATE_DATA:
                if (c == 0xFF) { /* IAC (Interpret As Command) */
                    current_state = TELNET_FILTER_STATE_IAC;
                    MB_LOG_DEBUG("TELNET filter: Detected IAC character");
                } else {
                    /* Normal data - pass through */
                    output[out_pos++] = c;
                }
                break;

            case TELNET_FILTER_STATE_IAC:
                if (c == 0xFF) {
                    /* Escaped IAC - pass through literal 0xFF */
                    output[out_pos++] = c;
                    current_state = TELNET_FILTER_STATE_DATA;
                } else if (c == 0xFB) { /* WILL */
                    current_state = TELNET_FILTER_STATE_WILL;
                    MB_LOG_DEBUG("TELNET filter: WILL option");
                } else if (c == 0xFC) { /* WONT */
                    current_state = TELNET_FILTER_STATE_WONT;
                    MB_LOG_DEBUG("TELNET filter: WONT option");
                } else if (c == 0xFD) { /* DO */
                    current_state = TELNET_FILTER_STATE_DO;
                    MB_LOG_DEBUG("TELNET filter: DO option");
                } else if (c == 0xFE) { /* DONT */
                    current_state = TELNET_FILTER_STATE_DONT;
                    MB_LOG_DEBUG("TELNET filter: DONT option");
                } else if (c == 0xFA) { /* SB (Suboption Begin) */
                    current_state = TELNET_FILTER_STATE_SB;
                    MB_LOG_DEBUG("TELNET filter: Suboption begin");
                } else {
                    /* Other IAC commands - filter out */
                    current_state = TELNET_FILTER_STATE_DATA;
                }
                break;

            case TELNET_FILTER_STATE_WILL:
            case TELNET_FILTER_STATE_WONT:
            case TELNET_FILTER_STATE_DO:
            case TELNET_FILTER_STATE_DONT:
                /* Option negotiation - filter out the option byte */
                current_state = TELNET_FILTER_STATE_DATA;
                break;

            case TELNET_FILTER_STATE_SB:
                if (c == 0xFF) {
                    /* Potential IAC to end suboption */
                    current_state = TELNET_FILTER_STATE_SB_DATA;
                }
                /* Filter out suboption data */
                break;

            case TELNET_FILTER_STATE_SB_DATA:
                if (c == 0xF0) { /* SE (Suboption End) */
                    current_state = TELNET_FILTER_STATE_DATA;
                    MB_LOG_DEBUG("TELNET filter: Suboption end");
                } else {
                    /* Still in suboption */
                    current_state = TELNET_FILTER_STATE_SB;
                }
                break;
        }
    }

    *output_len = out_pos;
    *state = current_state;

    return L3_SUCCESS;
}

/* ========== Scheduling and Fairness ========== */

/* Enhanced scheduling and fairness functions */

/* ========== Scheduling Functions (moved to level3_schedule.c) ========== */
/*
 * The following scheduling and fairness functions have been moved to level3_schedule.c:
 * - l3_init_enhanced_scheduling()
 * - l3_schedule_next_pipeline()
 * - l3_process_pipeline_with_quantum()
 * - l3_update_latency_stats()
 * - l3_is_direction_starving()
 * - l3_calculate_optimal_quantum()
 * - l3_update_fair_queue_weights()
 * - l3_get_scheduling_statistics()
 * - l3_enforce_latency_boundaries()
 * - l3_detect_latency_violation()
 * - l3_calculate_adaptive_quantum_with_latency()
 * - l3_update_direction_priorities()
 * - l3_get_direction_wait_time()
 * - l3_should_force_direction_switch()
 * - l3_switch_active_pipeline()
 * - l3_can_switch_pipeline()
 */

/* ========== Backpressure Management ========== */

int l3_apply_backpressure(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (!pipeline->backpressure_active) {
        pipeline->backpressure_active = true;
        pipeline->backpressure_start = time(NULL);
        pipeline->state = L3_PIPELINE_STATE_BLOCKED;
        MB_LOG_INFO("Pipeline %s: Backpressure applied", pipeline->name);
    }

    return L3_SUCCESS;
}

int l3_release_backpressure(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return L3_ERROR_INVALID_PARAM;
    }

    if (pipeline->backpressure_active) {
        pipeline->backpressure_active = false;
        pipeline->state = L3_PIPELINE_STATE_ACTIVE;
        MB_LOG_INFO("Pipeline %s: Backpressure released", pipeline->name);
    }

    return L3_SUCCESS;
}

/* ========== Half-duplex Control (moved to level3_schedule.c) ========== */
/*
 * l3_switch_active_pipeline() and l3_can_switch_pipeline() have been moved to level3_schedule.c
 */
/* ========== Statistics and Monitoring ========== */

void l3_print_stats(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return;
    }

    time_t uptime = time(NULL) - l3_ctx->start_time;

    MB_LOG_INFO("=== Level 3 Statistics ===");
    MB_LOG_INFO("System Uptime: %ld seconds", (long)uptime);
    MB_LOG_INFO("Total Pipeline Switches: %llu", (unsigned long long)l3_ctx->total_pipeline_switches);
    MB_LOG_INFO("System Utilization: %.2f%%", l3_ctx->system_utilization_pct);
    MB_LOG_INFO("Half-duplex Mode: %s", l3_ctx->half_duplex_mode ? "Enabled" : "Disabled");
    MB_LOG_INFO("Level 1 Ready: %s, Level 2 Ready: %s",
                l3_ctx->level1_ready ? "Yes" : "No",
                l3_ctx->level2_ready ? "Yes" : "No");
    MB_LOG_INFO("Scheduling Rounds: %d", l3_ctx->round_robin_counter);

    /* Print individual pipeline statistics */
    l3_print_pipeline_stats(&l3_ctx->pipeline_serial_to_telnet);
    l3_print_pipeline_stats(&l3_ctx->pipeline_telnet_to_serial);

    MB_LOG_INFO("==========================");
}

void l3_print_pipeline_stats(l3_pipeline_t *pipeline)
{
    if (pipeline == NULL) {
        return;
    }

    MB_LOG_INFO("Pipeline %s:", pipeline->name);
    MB_LOG_INFO("  State: %s", l3_pipeline_state_to_string(pipeline->state));
    MB_LOG_INFO("  Bytes Processed: %llu", (unsigned long long)pipeline->total_bytes_processed);
    MB_LOG_INFO("  Bytes Dropped: %llu", (unsigned long long)pipeline->total_bytes_dropped);
    MB_LOG_INFO("  Buffer Switches: %llu", (unsigned long long)pipeline->pipeline_switches);
    MB_LOG_INFO("  Avg Processing Time: %.2f ms", pipeline->avg_processing_time_ms);
    MB_LOG_INFO("  Backpressure Active: %s", pipeline->backpressure_active ? "Yes" : "No");
    MB_LOG_INFO("  Main Buffer Available: %zu bytes", l3_double_buffer_available(&pipeline->buffers));
    MB_LOG_INFO("  Sub Buffer Free: %zu bytes", l3_double_buffer_free(&pipeline->buffers));
}

double l3_get_system_utilization(l3_context_t *l3_ctx)
{
    if (l3_ctx == NULL) {
        return 0.0;
    }

    /* Calculate utilization based on processing time vs total time */
    time_t total_time = time(NULL) - l3_ctx->start_time;
    if (total_time == 0) {
        return 0.0;
    }

    /* Estimate utilization from pipeline activity */
    double total_processing_time = l3_ctx->pipeline_serial_to_telnet.avg_processing_time_ms +
                                  l3_ctx->pipeline_telnet_to_serial.avg_processing_time_ms;

    double utilization = (total_processing_time / (total_time * 1000.0)) * 100.0;
    if (utilization > 100.0) {
        utilization = 100.0;
    }

    l3_ctx->system_utilization_pct = utilization;
    return utilization;
}

/* ========== Thread Functions ========== */

void *l3_management_thread_func(void *arg)
{
    l3_context_t *l3_ctx = (l3_context_t *)arg;

    MB_LOG_INFO("Level 3 management thread started");

    while (l3_ctx->thread_running) {
        /* Process state machine - handles all transitions automatically */
        int ret = l3_process_state_machine(l3_ctx);
        if (ret != L3_SUCCESS) {
            MB_LOG_ERROR("State machine processing failed: %d", ret);
            /* Continue running even if state machine fails */
        }

        /* Only process data if in DATA_TRANSFER state */
        if (l3_ctx->system_state == L3_STATE_DATA_TRANSFER && l3_ctx->level3_active) {
            /* DEBUG: Log entry into data processing (only once at start) */
            static bool entered_data_transfer = false;
            if (!entered_data_transfer) {
                printf("[DEBUG-MGMT-THREAD] Entered DATA_TRANSFER processing loop\n");
                fflush(stdout);
                entered_data_transfer = true;
            }

            /* Use enhanced quantum-based scheduling instead of basic round-robin */
            int ret = l3_process_pipeline_with_quantum(l3_ctx, l3_ctx->active_pipeline);

            if (ret != L3_SUCCESS) {
                MB_LOG_WARNING("Quantum-based pipeline processing failed: %d", ret);
                /* Fall back to basic scheduling if quantum processing fails */
                l3_pipeline_direction_t next_pipeline = l3_schedule_next_pipeline(l3_ctx);

                /* Switch if needed */
                if (next_pipeline != l3_ctx->active_pipeline) {
                    l3_switch_active_pipeline(l3_ctx, next_pipeline);

                    /* Update fair queue weights when switching pipelines */
                    l3_update_fair_queue_weights(l3_ctx);
                }

                /* Process active pipeline */
                l3_pipeline_t *active = (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) ?
                                       &l3_ctx->pipeline_serial_to_telnet :
                                       &l3_ctx->pipeline_telnet_to_serial;

                /* Check for backpressure */
                if (l3_should_apply_backpressure(active)) {
                    l3_apply_backpressure(active);
                    usleep(10000);  /* 10ms delay under backpressure */
                    continue;
                } else {
                    l3_release_backpressure(active);
                }

                /* Process data based on pipeline direction using helper functions */
                if (l3_ctx->active_pipeline == L3_PIPELINE_SERIAL_TO_TELNET) {
                    l3_process_serial_to_telnet_chunk(l3_ctx);
                } else {
                    l3_process_telnet_to_serial_chunk(l3_ctx);
                }
            }

            /* Periodically update scheduling statistics and fair queue weights */
            static int statistics_counter = 0;
            if (++statistics_counter >= 100) {  /* Every 100 iterations */
                l3_scheduling_stats_t stats;
                if (l3_get_scheduling_statistics(l3_ctx, &stats) == L3_SUCCESS) {
                    MB_LOG_DEBUG("Scheduling stats - utilization: %.2f%%, fairness: %.2f, cycles: %d",
                                stats.system_utilization * 100.0, stats.fairness_ratio,
                                stats.total_scheduling_cycles);
                }
                l3_update_fair_queue_weights(l3_ctx);

                /* Periodic latency boundary enforcement for continuous monitoring */
                l3_enforce_latency_boundaries(l3_ctx);

                statistics_counter = 0;
            }
        } else {
            /* Not in data transfer state - wait for state changes */
            if (l3_ctx->system_state == L3_STATE_INITIALIZING) {
                usleep(100000);  /* 100ms during initialization */
            } else if (l3_ctx->system_state == L3_STATE_READY) {
                usleep(500000);  /* 500ms in ready state (DCD waiting) */
            } else if (l3_ctx->system_state == L3_STATE_CONNECTING ||
                      l3_ctx->system_state == L3_STATE_NEGOTIATING) {
                usleep(200000);  /* 200ms during connection/negotiation */
            } else if (l3_ctx->system_state == L3_STATE_FLUSHING) {
                usleep(50000);   /* 50ms during flushing (more frequent checks) */
            } else if (l3_ctx->system_state == L3_STATE_SHUTTING_DOWN ||
                      l3_ctx->system_state == L3_STATE_TERMINATED) {
                usleep(1000000); /* 1s during shutdown/termination */
            } else {
                usleep(L3_FAIRNESS_TIME_SLICE_MS * 1000);  /* Default timeslice */
            }
        }

        /* Update system utilization */
        l3_get_system_utilization(l3_ctx);
    }

    MB_LOG_INFO("Level 3 management thread exiting");
    return NULL;
}

/* ========== Utility Functions ========== */

/* l3_get_pipeline_name moved to level3_util.c */

/* Utility functions moved to level3_util.c */

/* ========== Enhanced Scheduling Helper Functions ========== */

/* l3_get_direction_name moved to level3_util.c - now exposed as public function */

/**
 * Process a chunk of data from serial to telnet
 * Implements line buffering based on Level 1 modem buffer logic:
 * 1. Accumulate data in line buffer
 * 2. Process complete lines through Hayes filter
 * 3. Forward filtered data to telnet pipeline
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_process_serial_to_telnet_chunk(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

    /* Static line buffer (persists across calls) */
    static unsigned char line_buffer[LINE_BUFFER_SIZE];
    static size_t line_len = 0;
    static time_t line_start_time = 0;

    /* Static multibyte buffer for character assembly */
    static unsigned char multibyte_buffer[6];  /* Max UTF-8 is 4 bytes, but safety margin */
    static size_t multibyte_len = 0;
    static int multibyte_expected = 0;
    static time_t multibyte_start_time = 0;

    /* Read from serial→telnet buffer (populated by modem thread) */
    unsigned char serial_buf[L3_MAX_BURST_SIZE];
    size_t serial_len = ts_cbuf_read(&l3_ctx->bridge->ts_serial_to_telnet_buf,
                                     serial_buf, sizeof(serial_buf));

    /* DEBUG: Log read result */
    static int read_counter = 0;
    if (++read_counter % 50 == 0 || serial_len > 0) {  /* Log periodically or when data found */
        /* Commented out to reduce log noise
        printf("[DEBUG-CHUNK-READ] Serial→Telnet buffer read: %zu bytes\n", serial_len);
        fflush(stdout);
        */
    }

    if (serial_len > 0) {
        time_t now = time(NULL);

        /* Check for line buffer timeout (20 seconds) */
        if (line_len > 0 && (now - line_start_time) > 20) {
            MB_LOG_DEBUG("Line buffer timeout - clearing old data");
            line_len = 0;
            memset(line_buffer, 0, sizeof(line_buffer));
        }

        /* Check for multibyte timeout (1 second) */
        if (multibyte_len > 0 && (now - multibyte_start_time) > 1) {
            MB_LOG_DEBUG("Multibyte timeout - echoing incomplete sequence: %zu bytes", multibyte_len);
            /* Echo incomplete multibyte as-is */
            l3_echo_to_modem(l3_ctx, multibyte_buffer, multibyte_len);

            /* Reset multibyte state */
            multibyte_len = 0;
            multibyte_expected = 0;
            memset(multibyte_buffer, 0, sizeof(multibyte_buffer));
        }

        /* Process input data byte by byte for line buffering and echo */
        for (size_t i = 0; i < serial_len; i++) {
            unsigned char c = serial_buf[i];

            /* Track start time for new line */
            if (line_len == 0) {
                line_start_time = now;
            }

            /* === Echo handling (immediate for single-byte, after assembly for multibyte) === */

            /* Check if we're in the middle of multibyte assembly */
            if (multibyte_len > 0) {
                /* Add to multibyte buffer */
                if (multibyte_len < sizeof(multibyte_buffer)) {
                    multibyte_buffer[multibyte_len++] = c;
                }

                /* Check if multibyte sequence is complete */
                if (l3_is_multibyte_complete(multibyte_buffer, multibyte_len, multibyte_expected)) {
                    /* Echo the complete multibyte character */
                    l3_echo_to_modem(l3_ctx, multibyte_buffer, multibyte_len);
                    MB_LOG_DEBUG("Echoed multibyte character: %d bytes", multibyte_len);

                    /* Reset multibyte buffer */
                    multibyte_len = 0;
                    multibyte_expected = 0;
                    memset(multibyte_buffer, 0, sizeof(multibyte_buffer));
                }
            } else if (l3_is_multibyte_start(c)) {
                /* Start of new multibyte sequence */
                multibyte_buffer[0] = c;
                multibyte_len = 1;
                multibyte_expected = l3_get_multibyte_length(c);
                multibyte_start_time = now;  /* Track when multibyte started */
                MB_LOG_DEBUG("Started multibyte sequence, expecting %d bytes", multibyte_expected);
            } else {
                /* Single-byte character - echo immediately */
                l3_echo_to_modem(l3_ctx, &c, 1);

                /* Note: CR/LF already echoed above as single bytes */
                /* No need for special newline handling here */
            }

            /* === Line buffer handling (for telnet transmission) === */

            /* Add character to line buffer */
            if (line_len < sizeof(line_buffer) - 1) {
                line_buffer[line_len++] = c;
                line_buffer[line_len] = '\0';
            }

            /* Check if we have a complete line */
            bool line_complete = false;
            bool buffer_full = false;

            if (c == '\r' || c == '\n') {
                line_complete = true;
                printf("[DEBUG-LINE] Line complete (CR/LF): %zu bytes\n", line_len);
            } else if (line_len >= sizeof(line_buffer) - 1) {
                buffer_full = true;
                printf("[DEBUG-LINE] Line buffer full: %zu bytes\n", line_len);
            }

            /* Process complete line or full buffer */
            if (line_complete || buffer_full) {
                /* Debug log the line content */
                printf("[DEBUG-LINE] Processing line: [%.*s]\n",
                       (int)(line_len > 50 ? 50 : line_len), line_buffer);
                fflush(stdout);

                /* Process through Hayes filter */
                unsigned char filtered_buf[L3_MAX_BURST_SIZE];
                size_t filtered_len = 0;

                /* Apply Hayes filter to the complete line */
                hayes_filter_context_t *hayes_ctx = &l3_ctx->pipeline_serial_to_telnet.filter_state.hayes_ctx;
                int filter_ret = l3_filter_hayes_commands(hayes_ctx,
                                                         line_buffer, line_len,
                                                         filtered_buf, sizeof(filtered_buf),
                                                         &filtered_len);

                /* DEBUG: Log filter result */
                printf("[DEBUG-FILTER] Hayes filter: in=%zu, out=%zu, ret=%d\n",
                       line_len, filtered_len, filter_ret);
                if (filtered_len > 0 && filtered_len <= 20) {
                    printf("[DEBUG-FILTER] Output: ");
                    for (size_t j = 0; j < filtered_len; j++) {
                        printf("%02x ", filtered_buf[j]);
                    }
                    printf("(%.*s)\n", (int)filtered_len, filtered_buf);
                }
                fflush(stdout);

                /* Send filtered data to telnet */
                if (filter_ret == L3_SUCCESS && filtered_len > 0) {
#ifdef ENABLE_LEVEL2
                    int send_ret = telnet_queue_write(&l3_ctx->bridge->telnet,
                                                     filtered_buf, filtered_len);
                    if (send_ret != SUCCESS) {
                        MB_LOG_WARNING("Failed to queue data to telnet: %d", send_ret);
                        printf("[ERROR] telnet_queue_write failed: %d\n", send_ret);
                        fflush(stdout);
                        /* Don't return error - continue processing */
                    } else {
                        printf("[DEBUG-TELNET-SEND] Queued %zu bytes to telnet buffer\n", filtered_len);
                        fflush(stdout);

                        /* Flush the write buffer to actually send data to telnet server */
                        int flush_ret = telnet_flush_writes(&l3_ctx->bridge->telnet);
                        if (flush_ret != SUCCESS) {
                            MB_LOG_WARNING("Failed to flush telnet write buffer: %d", flush_ret);
                            printf("[ERROR] telnet_flush_writes failed: %d\n", flush_ret);
                            fflush(stdout);
                        } else {
                            printf("[DEBUG-TELNET-FLUSH] Flushed telnet write buffer successfully\n");
                            fflush(stdout);
                            MB_LOG_DEBUG("Line processed and sent: %zu → %zu bytes to telnet",
                                       line_len, filtered_len);
                        }
                    }
#endif
                }

                /* Clear line buffer for next line */
                line_len = 0;
                memset(line_buffer, 0, sizeof(line_buffer));

                /* If buffer was full and not line complete,
                   start new line with current character */
                if (buffer_full && !line_complete) {
                    line_buffer[0] = c;
                    line_len = 1;
                    line_start_time = now;
                }
            }
        }

        /* Update pipeline statistics */
        l3_update_pipeline_stats(&l3_ctx->pipeline_serial_to_telnet,
                                serial_len, 0.0);
    }

    return L3_SUCCESS;  /* No data available is not an error */
}

/**
 * Process a chunk of data from telnet to serial
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
static int l3_process_telnet_to_serial_chunk(l3_context_t *l3_ctx)
{
    if (!l3_ctx) {
        return L3_ERROR_INVALID_PARAM;
    }

#ifdef ENABLE_LEVEL2
    /* Read from telnet buffer */
    unsigned char telnet_buf[L3_MAX_BURST_SIZE];
    size_t telnet_len = ts_cbuf_read(&l3_ctx->bridge->ts_telnet_to_serial_buf,
                                    telnet_buf, sizeof(telnet_buf));

    if (telnet_len > 0) {
        /* Process through pipeline */
        unsigned char filtered_buf[L3_MAX_BURST_SIZE];
        size_t filtered_len;

        int ret = l3_pipeline_process(&l3_ctx->pipeline_telnet_to_serial,
                                     telnet_buf, telnet_len,
                                     filtered_buf, sizeof(filtered_buf), &filtered_len);

        if (ret == L3_SUCCESS && filtered_len > 0) {
            /* Write to serial port */
            ssize_t sent = serial_write(&l3_ctx->bridge->serial,
                                       filtered_buf, filtered_len);
            if (sent > 0) {
                MB_LOG_DEBUG("Processed telnet→serial chunk: %zu → %zd bytes", telnet_len, sent);
                return L3_SUCCESS;
            } else {
                MB_LOG_WARNING("Failed to write to serial port");
                return L3_ERROR_IO;
            }
        }
        return ret;
    }
#endif

    return L3_SUCCESS;  /* No data available is not an error */
}

/* ========== Internal Helper Functions ========== */

static void l3_update_pipeline_stats(l3_pipeline_t *pipeline, size_t bytes_processed, double processing_time)
{
    if (pipeline == NULL) {
        return;
    }

    pipeline->total_bytes_processed += bytes_processed;
    pipeline->bytes_in_timeslice += bytes_processed;

    /* Update moving average of processing time */
    if (pipeline->avg_processing_time_ms == 0.0) {
        pipeline->avg_processing_time_ms = processing_time;
    } else {
        /* Exponential moving average with alpha = 0.1 */
        pipeline->avg_processing_time_ms = 0.9 * pipeline->avg_processing_time_ms + 0.1 * processing_time;
    }

    pipeline->last_activity = time(NULL);
}

/* ========== Latency Bound Guarantee Functions ========== */

/**
 * Enforce latency boundaries for both pipeline directions
 * This is the main function for implementing "지연 상한 보장" (latency bound guarantee)
 * @param l3_ctx Level 3 context
 * @return SUCCESS on success, error code on failure
 */
