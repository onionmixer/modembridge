/*
 * telnet_test.h - Level 2 Telnet test functionality
 *
 * Temporary test module for Level 2 telnet functionality.
 * Provides automated text transmission for testing purposes.
 * Only enabled when ENABLE_TELNET_TEST is defined (level2 build mode only).
 */

#ifndef MODEMBRIDGE_TELNET_TEST_H
#define MODEMBRIDGE_TELNET_TEST_H

#if defined(ENABLE_LEVEL2) && defined(ENABLE_TELNET_TEST)

#include "telnet.h"
#include <stdbool.h>

/* Telnet test context */
typedef struct {
    telnet_t *telnet;                    /* Telnet connection */
    bool enabled;                        /* Test enabled flag */
    bool running;                         /* Test running flag */

    /* Test configuration */
    char test_strings[3][32];          /* Test strings: "abcd", "한글", "こんにちは。" */
    int string_count;                   /* Number of test strings */
    int interval_seconds;              /* Interval between transmissions */

    /* Test state */
    int current_string_index;          /* Current string to send */
    time_t last_transmission;          /* Last transmission time */

    /* Statistics */
    int total_transmissions;           /* Total transmissions attempted */
    int successful_transmissions;       /* Successful transmissions */
    int failed_transmissions;           /* Failed transmissions */

    /* Logging */
    bool verbose_logging;               /* Enable verbose test logging */
} telnet_test_t;

/* Function prototypes */

/**
 * Initialize telnet test context
 * @param test Test context to initialize
 * @param telnet Telnet connection to use for testing
 * @return SUCCESS on success, error code on failure
 */
int telnet_test_init(telnet_test_t *test, telnet_t *telnet);

/**
 * Start telnet test functionality
 * @param test Test context
 * @return SUCCESS on success, error code on failure
 */
int telnet_test_start(telnet_test_t *test);

/**
 * Stop telnet test functionality
 * @param test Test context
 * @return SUCCESS on success, error code on failure
 */
int telnet_test_stop(telnet_test_t *test);

/**
 * Process telnet test (should be called regularly)
 * @param test Test context
 * @return SUCCESS on success, error code on failure
 */
int telnet_test_process(telnet_test_t *test);

/**
 * Check if telnet test is running
 * @param test Test context
 * @return true if running, false otherwise
 */
bool telnet_test_is_running(telnet_test_t *test);

/**
 * Get test statistics
 * @param test Test context
 * @param total Pointer to store total transmissions (can be NULL)
 * @param successful Pointer to store successful transmissions (can be NULL)
 * @param failed Pointer to store failed transmissions (can be NULL)
 */
void telnet_test_get_stats(telnet_test_t *test, int *total, int *successful, int *failed);

/**
 * Enable/disable verbose logging
 * @param test Test context
 * @param verbose Enable verbose logging
 */
void telnet_test_set_verbose(telnet_test_t *test, bool verbose);

/**
 * Configure test interval
 * @param test Test context
 * @param interval_seconds Interval between transmissions in seconds
 */
void telnet_test_set_interval(telnet_test_t *test, int interval_seconds);

#endif /* ENABLE_LEVEL2 && ENABLE_TELNET_TEST */

#endif /* MODEMBRIDGE_TELNET_TEST_H */