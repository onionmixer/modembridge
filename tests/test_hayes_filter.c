/*
 * test_hayes_filter.c - Test program for Hayes filter functionality
 * Tests both COMMAND mode and ONLINE mode filtering
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include "level3.h"

/* Test colors for output */
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_RESET   "\x1b[0m"

/* Function prototype from level3.c */
extern int l3_filter_hayes_commands(hayes_filter_context_t *ctx, const unsigned char *input, size_t input_len,
                                   unsigned char *output, size_t output_size, size_t *output_len);

/* Helper function to print hex dump */
void print_hex_dump(const char *label, const unsigned char *data, size_t len)
{
    printf("%s (%zu bytes): ", label, len);
    if (len == 0) {
        printf("(empty)");
    } else {
        for (size_t i = 0; i < len && i < 50; i++) {
            if (data[i] >= 32 && data[i] < 127) {
                printf("%c", data[i]);
            } else {
                printf("\\x%02x", data[i]);
            }
        }
        if (len > 50) printf("...");
    }
    printf("\n");
}

/* Test case structure */
typedef struct {
    const char *name;
    bool online_mode;
    const char *input;
    const char *expected_output;
    bool should_block;
} test_case_t;

/* Run a single test case */
bool run_test(hayes_filter_context_t *ctx, test_case_t *test)
{
    unsigned char output[1024];
    size_t output_len = 0;

    /* Set mode */
    ctx->in_online_mode = test->online_mode;
    if (!test->online_mode) {
        ctx->state = HAYES_STATE_NORMAL;
    }

    /* Clear buffers */
    ctx->line_len = 0;
    ctx->command_len = 0;
    ctx->plus_count = 0;
    memset(ctx->line_buffer, 0, sizeof(ctx->line_buffer));
    memset(ctx->command_buffer, 0, sizeof(ctx->command_buffer));

    printf("\n%sTEST: %s%s\n", COLOR_YELLOW, test->name, COLOR_RESET);
    printf("Mode: %s\n", test->online_mode ? "ONLINE" : "COMMAND");
    print_hex_dump("Input", (unsigned char *)test->input, strlen(test->input));

    /* Process input */
    int ret = l3_filter_hayes_commands(ctx,
                                      (unsigned char *)test->input, strlen(test->input),
                                      output, sizeof(output), &output_len);

    if (ret != 0) {
        printf("%sERROR: Filter returned %d%s\n", COLOR_RED, ret, COLOR_RESET);
        return false;
    }

    /* Check output */
    print_hex_dump("Output", output, output_len);

    bool blocked = (output_len == 0 || output_len < strlen(test->input));

    if (test->should_block) {
        if (!blocked) {
            printf("%sFAILED: Expected blocking but data passed through%s\n", COLOR_RED, COLOR_RESET);
            return false;
        }
        printf("%sPASSED: AT command blocked as expected%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        if (blocked) {
            printf("%sFAILED: Data was blocked unexpectedly%s\n", COLOR_RED, COLOR_RESET);
            return false;
        }

        if (test->expected_output) {
            if (output_len != strlen(test->expected_output) ||
                memcmp(output, test->expected_output, output_len) != 0) {
                printf("%sFAILED: Output mismatch%s\n", COLOR_RED, COLOR_RESET);
                print_hex_dump("Expected", (unsigned char *)test->expected_output, strlen(test->expected_output));
                return false;
            }
        }
        printf("%sPASSED: Data passed through correctly%s\n", COLOR_GREEN, COLOR_RESET);
    }

    return true;
}

/* Test +++ escape sequence */
bool test_escape_sequence(hayes_filter_context_t *ctx)
{
    printf("\n%s=== TESTING +++ ESCAPE SEQUENCE ===%s\n", COLOR_BLUE, COLOR_RESET);

    ctx->in_online_mode = true;
    ctx->line_len = 0;
    ctx->plus_count = 0;

    unsigned char output[1024];
    size_t output_len;

    /* Test 1: +++ with proper guard time */
    printf("\nTest: +++ with 1 second guard time\n");

    /* First character after 1 second pause */
    ctx->last_char_time = 0;  /* Simulate old timestamp */
    l3_filter_hayes_commands(ctx, (unsigned char *)"+", 1, output, sizeof(output), &output_len);
    printf("After first +: output_len=%zu, plus_count=%d\n", output_len, ctx->plus_count);

    /* Second + immediately */
    l3_filter_hayes_commands(ctx, (unsigned char *)"+", 1, output, sizeof(output), &output_len);
    printf("After second +: output_len=%zu, plus_count=%d\n", output_len, ctx->plus_count);

    /* Third + immediately */
    l3_filter_hayes_commands(ctx, (unsigned char *)"+", 1, output, sizeof(output), &output_len);
    printf("After third +: output_len=%zu, plus_count=%d, online_mode=%d\n",
           output_len, ctx->plus_count, ctx->in_online_mode);

    if (ctx->in_online_mode) {
        printf("%sFAILED: Should have switched to COMMAND mode%s\n", COLOR_RED, COLOR_RESET);
        return false;
    }
    printf("%sPASSED: Switched to COMMAND mode%s\n", COLOR_GREEN, COLOR_RESET);

    /* Test 2: +++ without guard time (should pass through) */
    printf("\nTest: +++ without guard time\n");
    ctx->in_online_mode = true;
    ctx->plus_count = 0;
    ctx->last_char_time = time(NULL) * 1000;  /* Recent timestamp */

    char *input = "+++";
    l3_filter_hayes_commands(ctx, (unsigned char *)input, strlen(input),
                            output, sizeof(output), &output_len);
    printf("Output: %.*s (len=%zu)\n", (int)output_len, output, output_len);

    if (output_len == 0) {
        printf("%sFAILED: +++ without guard time was blocked%s\n", COLOR_RED, COLOR_RESET);
        return false;
    }
    printf("%sPASSED: +++ without guard time passed through%s\n", COLOR_GREEN, COLOR_RESET);

    return true;
}

int main(int argc, char *argv[])
{
    printf("%s=== HAYES FILTER TEST PROGRAM ===%s\n", COLOR_BLUE, COLOR_RESET);

    /* Initialize filter context */
    hayes_filter_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = HAYES_STATE_NORMAL;
    ctx.dict = NULL;  /* Will use default dictionary */

    /* Define test cases */
    test_case_t tests[] = {
        /* COMMAND mode tests */
        {"AT command in COMMAND mode", false, "AT\r", "", true},
        {"ATZ command in COMMAND mode", false, "ATZ\r", "", true},
        {"Normal text in COMMAND mode", false, "Hello World\r", "Hello World\r", false},
        {"Email in COMMAND mode", false, "onionmixer@gmail.com\r", "onionmixer@gmail.com\r", false},

        /* ONLINE mode tests - complete lines */
        {"AT command in ONLINE mode (complete line)", true, "AT\r", "", true},
        {"ATH command in ONLINE mode", true, "ATH\r", "", true},
        {"AT+CGMI command in ONLINE mode", true, "AT+CGMI\r", "", true},
        {"Normal text in ONLINE mode", true, "Hello World\r", "Hello World\r", false},
        {"Email in ONLINE mode", true, "onionmixer@gmail.com\r", "onionmixer@gmail.com\r", false},

        /* Edge cases */
        {"Just 'A' in ONLINE mode", true, "A\r", "A\r", false},
        {"Just 'AT' without CR", true, "AT", "AT", false},  /* No line ending - should buffer */
        {"Text starting with 'At' (lowercase)", true, "Athens Greece\r", "Athens Greece\r", false},
        {"AT in middle of line", true, "CHAT ROOM\r", "CHAT ROOM\r", false},

        /* Multi-line test */
        {"Multi-line with AT command", true, "Hello\rAT\rWorld\r", "Hello\rWorld\r", false},
    };

    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    int failed = 0;

    /* Run basic tests */
    printf("\n%s=== RUNNING BASIC TESTS ===%s\n", COLOR_BLUE, COLOR_RESET);
    for (int i = 0; i < num_tests; i++) {
        if (run_test(&ctx, &tests[i])) {
            passed++;
        } else {
            failed++;
        }
    }

    /* Test escape sequence */
    if (test_escape_sequence(&ctx)) {
        passed++;
    } else {
        failed++;
    }

    /* Test character-by-character input (simulating real modem) */
    printf("\n%s=== TESTING CHARACTER-BY-CHARACTER INPUT ===%s\n", COLOR_BLUE, COLOR_RESET);

    /* Reset context for ONLINE mode */
    memset(&ctx, 0, sizeof(ctx));
    ctx.in_online_mode = true;

    const char *email = "onionmixer@gmail.com\r";
    unsigned char output[1024];
    unsigned char total_output[1024];
    size_t total_len = 0;

    printf("Sending email character by character: %s\n", email);

    for (size_t i = 0; i < strlen(email); i++) {
        size_t output_len = 0;
        l3_filter_hayes_commands(&ctx,
                                (unsigned char *)&email[i], 1,
                                output, sizeof(output), &output_len);

        if (output_len > 0) {
            memcpy(total_output + total_len, output, output_len);
            total_len += output_len;
        }

        printf("  Char '%c' -> output_len=%zu\n",
               email[i] >= 32 ? email[i] : '?', output_len);
    }

    printf("Total output: %.*s (len=%zu)\n", (int)total_len, total_output, total_len);

    if (total_len == strlen(email) && memcmp(total_output, email, total_len) == 0) {
        printf("%sPASSED: Character-by-character email test%s\n", COLOR_GREEN, COLOR_RESET);
        passed++;
    } else {
        printf("%sFAILED: Character-by-character email test%s\n", COLOR_RED, COLOR_RESET);
        failed++;
    }

    /* Print summary */
    printf("\n%s=== TEST SUMMARY ===%s\n", COLOR_BLUE, COLOR_RESET);
    printf("Total tests: %d\n", passed + failed);
    printf("%sPassed: %d%s\n", COLOR_GREEN, passed, COLOR_RESET);
    printf("%sFailed: %d%s\n", failed > 0 ? COLOR_RED : COLOR_GREEN, failed, COLOR_RESET);

    return failed > 0 ? 1 : 0;
}