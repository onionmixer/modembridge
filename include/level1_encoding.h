/*
 * level1_encoding.h - Character encoding and ANSI processing for Level 1
 *
 * This file contains declarations for UTF-8 validation and ANSI escape
 * sequence filtering functions used by Level 1 (Serial/Modem) components.
 */

#ifndef MODEMBRIDGE_LEVEL1_ENCODING_H
#define MODEMBRIDGE_LEVEL1_ENCODING_H

#include "level1_types.h"
#include "common.h"    /* For SUCCESS, ERROR_* codes */
#include <stddef.h>
#include <stdbool.h>

/* ========== UTF-8 Character Functions ========== */

/**
 * Check if byte is the start of a multibyte UTF-8 sequence
 * @param byte Byte to check
 * @return true if this is a UTF-8 start byte, false otherwise
 */
bool is_utf8_start(unsigned char byte);

/**
 * Check if byte is a UTF-8 continuation byte
 * @param byte Byte to check
 * @return true if this is a continuation byte (10xxxxxx), false otherwise
 */
bool is_utf8_continuation(unsigned char byte);

/**
 * Get expected length of UTF-8 sequence from first byte
 * @param byte First byte of potential UTF-8 sequence
 * @return Expected sequence length (1-4), or 0 if invalid
 */
int utf8_sequence_length(unsigned char byte);

/**
 * Validate a complete UTF-8 sequence
 * @param seq Pointer to sequence of bytes
 * @param len Length of sequence to validate
 * @return true if valid UTF-8 sequence, false otherwise
 */
bool is_valid_utf8_sequence(const unsigned char *seq, size_t len);

/* ========== ANSI Escape Sequence Processing ========== */

/**
 * Filter ANSI escape sequences from modem to telnet
 *
 * This function removes ANSI cursor control and screen manipulation sequences
 * from data going from the modem to the telnet server, while preserving
 * text content and multibyte characters.
 *
 * @param input Input buffer containing data with potential ANSI sequences
 * @param input_len Length of input data
 * @param output Output buffer for filtered data
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @param state Pointer to ANSI parser state (maintained across calls)
 * @return SUCCESS on success, ERROR_INVALID_ARG on invalid parameters
 */
int ansi_filter_modem_to_telnet(const unsigned char *input, size_t input_len,
                                unsigned char *output, size_t output_size,
                                size_t *output_len, ansi_state_t *state);

/**
 * Pass through data from telnet to modem (with ANSI sequences intact)
 *
 * This function passes data from telnet to modem without filtering,
 * allowing ANSI sequences from the server to reach the terminal client.
 *
 * @param input Input buffer containing telnet data
 * @param input_len Length of input data
 * @param output Output buffer for modem
 * @param output_size Size of output buffer
 * @param output_len Pointer to store actual output length
 * @return SUCCESS on success, ERROR_INVALID_ARG on invalid parameters
 */
int ansi_passthrough_telnet_to_modem(const unsigned char *input, size_t input_len,
                                     unsigned char *output, size_t output_size,
                                     size_t *output_len);

/* ========== Helper Functions ========== */

/**
 * Check if a character is printable ASCII
 * @param c Character to check
 * @return true if printable (0x20-0x7E), false otherwise
 */
static inline bool is_printable_ascii(unsigned char c)
{
    return (c >= 0x20 && c <= 0x7E);
}

/**
 * Check if a character is a control character
 * @param c Character to check
 * @return true if control character (0x00-0x1F or 0x7F), false otherwise
 */
static inline bool is_control_char(unsigned char c)
{
    return (c < 0x20 || c == 0x7F);
}

#endif /* MODEMBRIDGE_LEVEL1_ENCODING_H */