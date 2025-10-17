/*
 * telnet_test.c - Level 2 Telnet test functionality implementation
 *
 * Temporary test module for Level 2 telnet functionality.
 * Provides automated text transmission for testing purposes.
 */

#include "telnet_test.h"
#include "common.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * Initialize telnet test context
 */
int telnet_test_init(telnet_test_t *test, telnet_t *telnet)
{
    if (test == NULL || telnet == NULL) {
        return ERROR_INVALID_ARG;
    }

    memset(test, 0, sizeof(telnet_test_t));

    /* Link telnet connection */
    test->telnet = telnet;

    /* Default configuration */
    test->enabled = true;
    test->running = false;
    test->interval_seconds = 3;  /* 3 seconds between transmissions */

    /* Set test strings */
    SAFE_STRNCPY(test->test_strings[0], "abcd", sizeof(test->test_strings[0]));
    SAFE_STRNCPY(test->test_strings[1], "한글", sizeof(test->test_strings[1]));
    SAFE_STRNCPY(test->test_strings[2], "こんにちは。", sizeof(test->test_strings[2]));
    test->string_count = 3;

    /* Initialize state */
    test->current_string_index = 0;
    test->last_transmission = 0;

    /* Initialize statistics */
    test->total_transmissions = 0;
    test->successful_transmissions = 0;
    test->failed_transmissions = 0;

    /* Default logging */
    test->verbose_logging = true;

    MB_LOG_INFO("Telnet test initialized: %d test strings, %d second interval",
               test->string_count, test->interval_seconds);
    MB_LOG_INFO("Test strings: \"%s\", \"%s\", \"%s\"",
               test->test_strings[0], test->test_strings[1], test->test_strings[2]);

    return SUCCESS;
}

/**
 * Start telnet test functionality
 */
int telnet_test_start(telnet_test_t *test)
{
    if (test == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!test->enabled) {
        MB_LOG_INFO("Telnet test is disabled");
        return SUCCESS;
    }

    if (test->running) {
        MB_LOG_WARNING("Telnet test already running");
        return SUCCESS;
    }

    test->running = true;
    test->current_string_index = 0;
    test->last_transmission = 0;
    test->total_transmissions = 0;
    test->successful_transmissions = 0;
    test->failed_transmissions = 0;

    MB_LOG_INFO("Telnet test started: will transmit every %d seconds", test->interval_seconds);
    if (test->verbose_logging) {
        printf("[TEST] === TELNET TEST STARTED ===\n");
        printf("[TEST] Will transmit to %s:%d\n", test->telnet->host, test->telnet->port);
        printf("[TEST] Interval: %d seconds\n", test->interval_seconds);
        printf("[TEST] Test strings: \"%s\", \"%s\", \"%s\"\n",
               test->test_strings[0], test->test_strings[1], test->test_strings[2]);
        printf("[TEST] ================================\n");
        fflush(stdout);
    }

    return SUCCESS;
}

/**
 * Stop telnet test functionality
 */
int telnet_test_stop(telnet_test_t *test)
{
    if (test == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!test->running) {
        return SUCCESS;
    }

    test->running = false;

    MB_LOG_INFO("Telnet test stopped after %d transmissions (%d successful, %d failed)",
               test->total_transmissions, test->successful_transmissions, test->failed_transmissions);

    if (test->verbose_logging) {
        printf("[TEST] === TELNET TEST STOPPED ===\n");
        printf("[TEST] Total transmissions: %d\n", test->total_transmissions);
        printf("[TEST] Successful: %d\n", test->successful_transmissions);
        printf("[TEST] Failed: %d\n", test->failed_transmissions);
        printf("[TEST] ================================\n");
        fflush(stdout);
    }

    return SUCCESS;
}

/**
 * Send test string via telnet
 */
static int telnet_test_send_string(telnet_test_t *test, const char *string)
{
    if (test == NULL || string == NULL || test->telnet == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!telnet_is_connected(test->telnet)) {
        if (test->verbose_logging) {
            printf("[TEST] Telnet not connected, cannot send: \"%s\"\n", string);
            fflush(stdout);
        }
        return ERROR_CONNECTION;
    }

    size_t len = strlen(string);
    ssize_t sent = telnet_send(test->telnet, string, len);

    if (sent > 0) {
        test->successful_transmissions++;
        MB_LOG_DEBUG("Telnet test sent: \"%s\" (%zd bytes)", string, sent);

        if (test->verbose_logging) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);

            printf("[TEST] [%s] Sent: \"%s\" (%zd bytes)\n", timestamp, string, sent);
            fflush(stdout);
        }
        return SUCCESS;
    } else {
        test->failed_transmissions++;
        MB_LOG_ERROR("Telnet test failed to send: \"%s\"", string);

        if (test->verbose_logging) {
            printf("[TEST] Failed to send: \"%s\"\n", string);
            fflush(stdout);
        }
        return ERROR_IO;
    }
}

/**
 * Process telnet test (should be called regularly)
 */
int telnet_test_process(telnet_test_t *test)
{
    if (test == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!test->enabled || !test->running) {
        return SUCCESS;
    }

    /* Check if telnet is connected */
    if (!telnet_is_connected(test->telnet)) {
        return SUCCESS;
    }

    time_t now = time(NULL);

    /* Check if it's time to send next string */
    if (test->last_transmission == 0 ||
        (now - test->last_transmission) >= test->interval_seconds) {

        /* Send current test string */
        const char *current_string = test->test_strings[test->current_string_index];
        int result = telnet_test_send_string(test, current_string);

        test->total_transmissions++;
        test->last_transmission = now;

        /* Move to next string */
        test->current_string_index = (test->current_string_index + 1) % test->string_count;

        if (result != SUCCESS && test->verbose_logging) {
            MB_LOG_ERROR("Telnet test transmission failed for string: \"%s\"", current_string);
        }
    }

    return SUCCESS;
}

/**
 * Check if telnet test is running
 */
bool telnet_test_is_running(telnet_test_t *test)
{
    return (test != NULL) && test->running;
}

/**
 * Get test statistics
 */
void telnet_test_get_stats(telnet_test_t *test, int *total, int *successful, int *failed)
{
    if (test == NULL) {
        return;
    }

    if (total) *total = test->total_transmissions;
    if (successful) *successful = test->successful_transmissions;
    if (failed) *failed = test->failed_transmissions;
}

/**
 * Enable/disable verbose logging
 */
void telnet_test_set_verbose(telnet_test_t *test, bool verbose)
{
    if (test != NULL) {
        test->verbose_logging = verbose;
        MB_LOG_DEBUG("Telnet test verbose logging: %s", verbose ? "enabled" : "disabled");
    }
}

/**
 * Configure test interval
 */
void telnet_test_set_interval(telnet_test_t *test, int interval_seconds)
{
    if (test != NULL && interval_seconds > 0) {
        test->interval_seconds = interval_seconds;
        MB_LOG_INFO("Telnet test interval set to %d seconds", interval_seconds);
    }
}