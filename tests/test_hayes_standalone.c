/*
 * test_hayes_standalone.c - Standalone test for Hayes filter
 * Extracts just the filter function for testing
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

/* Color codes for output */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_RESET   "\x1b[0m"

/* Filter states */
typedef enum {
    HAYES_STATE_NORMAL = 0,
    HAYES_STATE_ESCAPE,
    HAYES_STATE_PLUS_ESCAPE,
    HAYES_STATE_COMMAND,
    HAYES_STATE_RESULT,
    HAYES_STATE_CR_WAIT,
    HAYES_STATE_LF_WAIT
} hayes_filter_state_t;

/* Simplified Hayes filter context */
typedef struct {
    hayes_filter_state_t state;
    unsigned char command_buffer[256];
    size_t command_len;
    unsigned char result_buffer[256];
    size_t result_len;

    /* Line buffering for complete commands */
    unsigned char line_buffer[256];
    size_t line_len;
    long long line_start_time;

    int plus_count;
    long long plus_start_time;
    long long last_char_time;
    bool in_online_mode;
} hayes_filter_context_t;

/* Get timestamp in milliseconds */
static long long get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Simplified Hayes filter - ONLINE mode with line buffering */
int hayes_filter_online(hayes_filter_context_t *ctx, const unsigned char *input, size_t input_len,
                        unsigned char *output, size_t output_size, size_t *output_len)
{
    size_t out_pos = 0;
    *output_len = 0;
    long long current_time = get_timestamp_ms();

    for (size_t i = 0; i < input_len && out_pos < output_size; i++) {
        unsigned char c = input[i];

        /* +++ escape sequence detection */
        if (c == '+') {
            if (ctx->plus_count == 0) {
                long long elapsed = current_time - ctx->last_char_time;
                if (elapsed >= 1000) { /* 1 second guard time */
                    ctx->plus_start_time = current_time;
                    ctx->plus_count = 1;
                    ctx->last_char_time = current_time;
                    continue; /* Buffer the + */
                }
            } else if (ctx->plus_count == 1 || ctx->plus_count == 2) {
                ctx->plus_count++;
                if (ctx->plus_count == 3) {
                    /* +++ detected */
                    printf("  [Filter: +++ escape detected, switching to COMMAND mode]\n");
                    ctx->in_online_mode = false;
                    ctx->state = HAYES_STATE_NORMAL;
                    ctx->plus_count = 0;
                    ctx->line_len = 0;
                    continue; /* Don't output +++ */
                }
                ctx->last_char_time = current_time;
                continue; /* Buffer more + */
            }
        } else if (ctx->plus_count > 0) {
            /* Not a + - flush buffered + characters */
            for (int j = 0; j < ctx->plus_count; j++) {
                if (out_pos < output_size) output[out_pos++] = '+';
            }
            ctx->plus_count = 0;
        }

        /* Line buffering for AT command detection */
        if (ctx->line_len == 0) {
            ctx->line_start_time = current_time;
        }

        /* Add to line buffer */
        if (ctx->line_len < sizeof(ctx->line_buffer) - 1) {
            ctx->line_buffer[ctx->line_len++] = c;
            ctx->line_buffer[ctx->line_len] = '\0';
        }

        /* Check for complete line */
        if (c == '\r' || c == '\n') {
            /* Process complete line */
            bool is_at_command = false;

            /* Check for AT command (minimum 3 chars: "AT\r") */
            if (ctx->line_len >= 3) {
                if ((ctx->line_buffer[0] == 'A' || ctx->line_buffer[0] == 'a') &&
                    (ctx->line_buffer[1] == 'T' || ctx->line_buffer[1] == 't')) {
                    is_at_command = true;
                    printf("  [Filter: AT command BLOCKED: %.20s]\n", ctx->line_buffer);
                }
            }

            if (!is_at_command) {
                /* Output the entire line */
                for (size_t j = 0; j < ctx->line_len; j++) {
                    if (out_pos < output_size) {
                        output[out_pos++] = ctx->line_buffer[j];
                    }
                }
            }
            /* Clear line buffer */
            ctx->line_len = 0;
        } else if (ctx->line_len >= sizeof(ctx->line_buffer) - 1) {
            /* Buffer overflow - flush */
            for (size_t j = 0; j < ctx->line_len; j++) {
                if (out_pos < output_size) {
                    output[out_pos++] = ctx->line_buffer[j];
                }
            }
            ctx->line_len = 0;
            ctx->line_buffer[0] = c;
            ctx->line_len = 1;
        }

        ctx->last_char_time = current_time;
    }

    *output_len = out_pos;
    return 0;
}

/* Test a single input scenario */
void test_input(const char *description, const char *input, bool online_mode)
{
    hayes_filter_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.in_online_mode = online_mode;
    ctx.state = HAYES_STATE_NORMAL;

    unsigned char output[1024];
    size_t output_len = 0;

    printf("\n%s=== Test: %s ===%s\n", COLOR_YELLOW, description, COLOR_RESET);
    printf("Mode: %s\n", online_mode ? "ONLINE" : "COMMAND");
    printf("Input: \"");
    for (size_t i = 0; i < strlen(input); i++) {
        if (input[i] == '\r') printf("\\r");
        else if (input[i] == '\n') printf("\\n");
        else printf("%c", input[i]);
    }
    printf("\" (%zu bytes)\n", strlen(input));

    if (online_mode) {
        hayes_filter_online(&ctx, (unsigned char *)input, strlen(input),
                          output, sizeof(output), &output_len);
    } else {
        printf("(COMMAND mode not implemented in this test)\n");
        return;
    }

    printf("Output: \"");
    for (size_t i = 0; i < output_len; i++) {
        if (output[i] == '\r') printf("\\r");
        else if (output[i] == '\n') printf("\\n");
        else printf("%c", output[i]);
    }
    printf("\" (%zu bytes)\n", output_len);

    /* Check result */
    if (strncmp(input, "AT", 2) == 0 && (input[2] == '\r' || input[2] == '\n' ||
        (input[2] >= 'A' && input[2] <= 'Z') || (input[2] >= '0' && input[2] <= '9'))) {
        /* AT command - should be blocked */
        if (output_len == 0) {
            printf("%sResult: PASSED - AT command blocked%s\n", COLOR_GREEN, COLOR_RESET);
        } else {
            printf("%sResult: FAILED - AT command not blocked%s\n", COLOR_RED, COLOR_RESET);
        }
    } else {
        /* Normal data - should pass through */
        if (output_len == strlen(input)) {
            printf("%sResult: PASSED - Data passed through%s\n", COLOR_GREEN, COLOR_RESET);
        } else {
            printf("%sResult: WARNING - Data length mismatch%s\n", COLOR_YELLOW, COLOR_RESET);
        }
    }
}

/* Test character-by-character input */
void test_char_by_char(const char *description, const char *input, bool online_mode)
{
    hayes_filter_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.in_online_mode = online_mode;
    ctx.state = HAYES_STATE_NORMAL;

    unsigned char output[1024];
    unsigned char total_output[1024];
    size_t total_len = 0;

    printf("\n%s=== Test (char-by-char): %s ===%s\n", COLOR_BLUE, description, COLOR_RESET);
    printf("Mode: %s\n", online_mode ? "ONLINE" : "COMMAND");
    printf("Input: \"%s\" (%zu bytes)\n", input, strlen(input));

    for (size_t i = 0; i < strlen(input); i++) {
        size_t output_len = 0;

        if (online_mode) {
            hayes_filter_online(&ctx, (unsigned char *)&input[i], 1,
                              output, sizeof(output), &output_len);
        }

        if (output_len > 0) {
            memcpy(total_output + total_len, output, output_len);
            total_len += output_len;
        }

        printf("  Char[%zu]: '%c' (0x%02x) -> %zu bytes out\n",
               i, input[i] >= 32 ? input[i] : '?', input[i], output_len);
    }

    printf("Total output: \"");
    for (size_t i = 0; i < total_len; i++) {
        if (total_output[i] == '\r') printf("\\r");
        else if (total_output[i] == '\n') printf("\\n");
        else printf("%c", total_output[i]);
    }
    printf("\" (%zu bytes)\n", total_len);

    /* Check result for email case */
    if (strstr(input, "@") != NULL) {
        if (total_len == strlen(input)) {
            printf("%sResult: PASSED - Email passed through completely%s\n", COLOR_GREEN, COLOR_RESET);
        } else {
            printf("%sResult: FAILED - Email data lost (expected %zu, got %zu)%s\n",
                   COLOR_RED, strlen(input), total_len, COLOR_RESET);
        }
    }
}

int main(void)
{
    printf("%s========================================%s\n", COLOR_BLUE, COLOR_RESET);
    printf("%s     HAYES FILTER STANDALONE TEST      %s\n", COLOR_BLUE, COLOR_RESET);
    printf("%s========================================%s\n", COLOR_BLUE, COLOR_RESET);

    /* Test complete line inputs */
    printf("\n%s--- COMPLETE LINE TESTS (ONLINE MODE) ---%s\n", COLOR_BLUE, COLOR_RESET);
    test_input("Normal email address", "onionmixer@gmail.com\r", true);
    test_input("AT command", "AT\r", true);
    test_input("ATH command", "ATH\r", true);
    test_input("AT+CGMI command", "AT+CGMI\r", true);
    test_input("Normal text", "Hello World\r", true);
    test_input("Text with 'AT' in middle", "CHAT ROOM\r", true);
    test_input("Text starting with 'At'", "Athens Greece\r", true);

    /* Test character-by-character input */
    printf("\n%s--- CHARACTER-BY-CHARACTER TESTS (ONLINE MODE) ---%s\n", COLOR_BLUE, COLOR_RESET);
    test_char_by_char("Email address", "onionmixer@gmail.com\r", true);
    test_char_by_char("AT command", "AT\r", true);
    test_char_by_char("Normal text", "Hello World\r", true);

    /* Test +++ escape sequence */
    printf("\n%s--- ESCAPE SEQUENCE TEST ---%s\n", COLOR_BLUE, COLOR_RESET);

    hayes_filter_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.in_online_mode = true;
    ctx.last_char_time = 0;  /* Old timestamp for guard time */

    unsigned char output[100];
    size_t output_len;

    printf("Testing +++ with guard time:\n");
    printf("  Sending '+' after 1 second pause...\n");
    hayes_filter_online(&ctx, (unsigned char *)"+", 1, output, sizeof(output), &output_len);
    printf("  plus_count=%d, output_len=%zu\n", ctx.plus_count, output_len);

    printf("  Sending second '+'...\n");
    hayes_filter_online(&ctx, (unsigned char *)"+", 1, output, sizeof(output), &output_len);
    printf("  plus_count=%d, output_len=%zu\n", ctx.plus_count, output_len);

    printf("  Sending third '+'...\n");
    hayes_filter_online(&ctx, (unsigned char *)"+", 1, output, sizeof(output), &output_len);
    printf("  plus_count=%d, output_len=%zu, online_mode=%d\n", ctx.plus_count, output_len, ctx.in_online_mode);

    if (!ctx.in_online_mode) {
        printf("%sResult: PASSED - Switched to COMMAND mode%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        printf("%sResult: FAILED - Still in ONLINE mode%s\n", COLOR_RED, COLOR_RESET);
    }

    printf("\n%s========================================%s\n", COLOR_BLUE, COLOR_RESET);
    printf("%s           TEST COMPLETED              %s\n", COLOR_BLUE, COLOR_RESET);
    printf("%s========================================%s\n", COLOR_BLUE, COLOR_RESET);

    return 0;
}