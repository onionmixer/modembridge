/*
 * level1_encoding.c - Character encoding and ANSI processing for Level 1
 *
 * This file contains implementations for UTF-8 validation and ANSI escape
 * sequence filtering functions used by Level 1 (Serial/Modem) components.
 */

#include "level1_encoding.h"
#include "common.h"
#include <string.h>
#include <stdbool.h>

/* For logging */
#ifdef MB_LOG_WARNING
  /* Use MB_LOG_WARNING if available */
#else
  #include <stdio.h>
  #define MB_LOG_WARNING(...) fprintf(stderr, "WARNING: " __VA_ARGS__)
#endif

/* ========== UTF-8 Character Functions ========== */

/**
 * Check if byte is start of multibyte UTF-8 sequence
 */
bool is_utf8_start(unsigned char byte)
{
    /* UTF-8 start bytes: 11xxxxxx (but not 11111110 or 11111111) */
    return (byte & 0xC0) == 0xC0 && (byte & 0xFE) != 0xFE;
}

/**
 * Check if byte is UTF-8 continuation byte
 */
bool is_utf8_continuation(unsigned char byte)
{
    /* UTF-8 continuation bytes: 10xxxxxx */
    return (byte & 0xC0) == 0x80;
}

/**
 * Get expected length of UTF-8 sequence from first byte
 */
int utf8_sequence_length(unsigned char byte)
{
    if ((byte & 0x80) == 0x00) {
        /* 0xxxxxxx - 1 byte (ASCII) */
        return 1;
    } else if ((byte & 0xE0) == 0xC0) {
        /* 110xxxxx - 2 bytes */
        return 2;
    } else if ((byte & 0xF0) == 0xE0) {
        /* 1110xxxx - 3 bytes */
        return 3;
    } else if ((byte & 0xF8) == 0xF0) {
        /* 11110xxx - 4 bytes */
        return 4;
    }

    /* Invalid UTF-8 start byte */
    return 0;
}

/**
 * Validate complete UTF-8 sequence
 */
bool is_valid_utf8_sequence(const unsigned char *seq, size_t len)
{
    if (seq == NULL || len == 0) {
        return false;
    }

    int expected_len = utf8_sequence_length(seq[0]);
    if (expected_len == 0 || (size_t)expected_len != len) {
        return false;
    }

    /* Check continuation bytes */
    for (size_t i = 1; i < len; i++) {
        if (!is_utf8_continuation(seq[i])) {
            return false;
        }
    }

    return true;
}

/* ========== ANSI Escape Sequence Processing ========== */

/**
 * Filter ANSI escape sequences from modem input
 *
 * This function removes ANSI control sequences while preserving text content.
 * It maintains state across calls to handle sequences that span buffer boundaries.
 */
int ansi_filter_modem_to_telnet(const unsigned char *input, size_t input_len,
                                unsigned char *output, size_t output_size,
                                size_t *output_len, ansi_state_t *state)
{
    size_t out_pos = 0;
    ansi_state_t current_state = state ? *state : ANSI_STATE_NORMAL;

    if (input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    *output_len = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];

        switch (current_state) {
            case ANSI_STATE_NORMAL:
                if (c == 0x1B) {  /* ESC */
                    current_state = ANSI_STATE_ESC;
                } else {
                    /* Normal character - pass through */
                    if (out_pos < output_size) {
                        output[out_pos++] = c;
                    } else {
                        /* Buffer full - log warning once */
                        static bool overflow_warned = false;
                        if (!overflow_warned) {
                            MB_LOG_WARNING("ANSI filter output buffer full - data truncated (multibyte chars may break)");
                            overflow_warned = true;
                        }
                    }
                }
                break;

            case ANSI_STATE_ESC:
                if (c == '[') {
                    /* CSI sequence start */
                    current_state = ANSI_STATE_CSI;
                } else if (c == 'c') {
                    /* ESC c - Reset command, filter out */
                    current_state = ANSI_STATE_NORMAL;
                } else {
                    /* Other escape sequences - filter out for now */
                    current_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_CSI:
                /* Check if this is a parameter or intermediate byte */
                if (c >= 0x30 && c <= 0x3F) {
                    /* Parameter bytes (0-9:;<=>?) */
                    current_state = ANSI_STATE_CSI_PARAM;
                } else if (c >= 0x40 && c <= 0x7E) {
                    /* Final byte - end of CSI sequence */
                    current_state = ANSI_STATE_NORMAL;
                } else {
                    /* Invalid CSI sequence - return to normal */
                    current_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_CSI_PARAM:
                if (c >= 0x30 && c <= 0x3F) {
                    /* More parameter bytes - stay in this state */
                } else if (c >= 0x40 && c <= 0x7E) {
                    /* Final byte - end of CSI sequence */
                    current_state = ANSI_STATE_NORMAL;
                } else {
                    /* Invalid - return to normal */
                    current_state = ANSI_STATE_NORMAL;
                }
                break;

            default:
                /* Unknown state - reset to normal */
                current_state = ANSI_STATE_NORMAL;
                break;
        }
    }

    *output_len = out_pos;

    /* Save state for next call if provided */
    if (state) {
        *state = current_state;
    }

    return SUCCESS;
}

/**
 * Pass through ANSI escape sequences from telnet to modem
 *
 * This function passes data through without filtering, allowing ANSI sequences
 * from the telnet server to reach the modem/terminal client.
 */
int ansi_passthrough_telnet_to_modem(const unsigned char *input, size_t input_len,
                                     unsigned char *output, size_t output_size,
                                     size_t *output_len)
{
    if (input == NULL || output == NULL || output_len == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Simple passthrough - copy as much as will fit */
    size_t copy_len = (input_len < output_size) ? input_len : output_size;
    memcpy(output, input, copy_len);
    *output_len = copy_len;

    return SUCCESS;
}