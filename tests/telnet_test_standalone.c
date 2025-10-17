/*
 * telnet_test_standalone.c - Standalone Level 2 Telnet Test
 *
 * Independent test program for Level 2 telnet functionality.
 * Tests telnet connection and multi-language text transmission
 * without requiring modem hardware or serial ports.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#include "common.h"
#include "telnet.h"
#include "telnet_test.h"

/* Global flag for signal handling */
static volatile sig_atomic_t test_running = 1;

/* Test context */
typedef struct {
    telnet_t telnet;
    telnet_test_t telnet_test;
    char host[256];
    int port;
    bool connected;
    time_t start_time;
    int test_duration;
    bool verbose;
} standalone_test_t;

/* Signal handler */
static void signal_handler(int sig)
{
    (void)sig;
    test_running = 0;
    printf("\n[INFO] Signal received, stopping test...\n");
}

/* Setup signal handlers */
static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
}

/* Connect to telnet server */
static int connect_to_server(standalone_test_t *test)
{
    printf("[INFO] Connecting to telnet server %s:%d\n", test->host, test->port);

    /* Initialize telnet */
    telnet_init(&test->telnet);

    /* Configure telnet for testing */
    telnet_set_keepalive(&test->telnet, true, 30, 120);
    telnet_set_error_handling(&test->telnet, true, 3, 10);

    /* Connect */
    int result = telnet_connect(&test->telnet, test->host, test->port);
    if (result != SUCCESS) {
        printf("[ERROR] Failed to connect to telnet server: %s\n",
               error_to_string(result));
        return result;
    }

    /* Wait for connection to complete */
    printf("[INFO] Waiting for connection to complete...\n");
    time_t connect_start = time(NULL);

    while (!telnet_is_connected(&test->telnet) &&
           (time(NULL) - connect_start) < 10) {

        /* Process events */
        if (telnet_process_events(&test->telnet, 100) == SUCCESS) {
            if (telnet_has_error(&test->telnet)) {
                printf("[ERROR] Connection error detected\n");
                return ERROR_CONNECTION;
            }
        }

        usleep(100000); /* 100ms */
    }

    if (!telnet_is_connected(&test->telnet)) {
        printf("[ERROR] Connection timeout\n");
        return ERROR_TIMEOUT;
    }

    printf("[INFO] Connected to telnet server successfully\n");
    test->connected = true;

    /* Initialize telnet test */
    telnet_test_init(&test->telnet_test, &test->telnet);
    telnet_test_set_verbose(&test->telnet_test, test->verbose);

    return SUCCESS;
}

/* Disconnect from telnet server */
static void disconnect_from_server(standalone_test_t *test)
{
    if (test->connected) {
        printf("[INFO] Disconnecting from telnet server\n");
        telnet_test_stop(&test->telnet_test);
        telnet_disconnect(&test->telnet);
        test->connected = false;
    }
}

/* Process telnet events */
static int process_telnet_events(standalone_test_t *test)
{
    int result = telnet_process_events(&test->telnet, 100);
    if (result != SUCCESS) {
        printf("[ERROR] Telnet event processing failed: %s\n",
               error_to_string(result));
        return result;
    }

    if (telnet_has_error(&test->telnet)) {
        printf("[WARNING] Telnet error detected, may need reconnection\n");
        return ERROR_CONNECTION;
    }

    /* Check connection health */
    result = telnet_check_connection_health(&test->telnet);
    if (result != SUCCESS) {
        printf("[WARNING] Telnet health check failed: %s\n",
               error_to_string(result));
        return result;
    }

    /* Handle read events */
    if (telnet_can_read(&test->telnet)) {
        unsigned char buffer[4096];
        size_t output_len = 0;

        result = telnet_process_reads(&test->telnet, buffer, sizeof(buffer), &output_len);
        if (result == SUCCESS && output_len > 0) {
            printf("[RECV] Received %zu bytes from telnet server: \"", output_len);
            for (size_t i = 0; i < output_len && i < 100; i++) {
                char c = buffer[i];
                if (c >= 32 && c <= 126) {
                    putchar(c);
                } else if (c == '\r') {
                    printf("\\r");
                } else if (c == '\n') {
                    printf("\\n");
                } else {
                    printf("\\x%02X", (unsigned char)c);
                }
            }
            if (output_len > 100) {
                printf("...(%zu more bytes)", output_len - 100);
            }
            printf("\"\n");

            /* This is the "timestamp reception" test - receiving text from server */
            time_t now = time(NULL);
            int elapsed = now - test->start_time;
            printf("[TIMESTAMP] %d seconds: Received data from telnet server\n", elapsed);

        } else if (result != SUCCESS) {
            printf("[ERROR] Failed to read from telnet: %s\n",
                   error_to_string(result));
            return result;
        }
    }

    /* Handle write events */
    if (telnet_can_write(&test->telnet)) {
        result = telnet_flush_writes(&test->telnet);
        if (result != SUCCESS) {
            printf("[ERROR] Failed to write to telnet: %s\n",
                   error_to_string(result));
            return result;
        }
    }

    return SUCCESS;
}

/* Run the standalone test */
static int run_standalone_test(standalone_test_t *test)
{
    printf("=== Standalone Level 2 Telnet Test ===\n");
    printf("Server: %s:%d\n", test->host, test->port);
    printf("Duration: %d seconds\n", test->test_duration);
    printf("Test strings: \"abcd\", \"한글\", \"こんにちは。\"\n");
    printf("Each string sent at 3-second intervals\n");
    printf("=========================================\n\n");

    /* Connect to server */
    int result = connect_to_server(test);
    if (result != SUCCESS) {
        return result;
    }

    /* Start telnet test */
    printf("[INFO] Starting telnet test transmission...\n");
    telnet_test_start(&test->telnet_test);

    /* Main test loop */
    test->start_time = time(NULL);

    while (test_running && (time(NULL) - test->start_time) < test->test_duration) {
        /* Process telnet events */
        result = process_telnet_events(test);
        if (result != SUCCESS) {
            printf("[ERROR] Telnet processing failed, disconnecting\n");
            break;
        }

        /* Process telnet test (handles the 3-second interval transmissions) */
        telnet_test_process(&test->telnet_test);

        /* Show elapsed time */
        int elapsed = time(NULL) - test->start_time;
        if (elapsed % 5 == 0 && elapsed > 0) {
            printf("[PROGRESS] Test running: %d/%d seconds\n",
                   elapsed, test->test_duration);
        }

        /* Small delay */
        usleep(100000); /* 100ms */
    }

    /* Get test statistics */
    int total, successful, failed;
    telnet_test_get_stats(&test->telnet_test, &total, &successful, &failed);

    printf("\n=== Test Results ===\n");
    printf("Duration: %ld seconds\n", time(NULL) - test->start_time);
    printf("Total transmissions: %d\n", total);
    printf("Successful: %d\n", successful);
    printf("Failed: %d\n", failed);

    if (failed == 0) {
        printf("Result: SUCCESS - All transmissions completed successfully\n");
        result = SUCCESS;
    } else {
        printf("Result: PARTIAL - %d transmissions failed\n", failed);
        result = ERROR_PARTIAL;
    }

    /* Disconnect */
    disconnect_from_server(test);

    return result;
}

/* Print usage */
static void print_usage(const char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("\nOptions:\n");
    printf("  -h HOST      Telnet server host (default: 127.0.0.1)\n");
    printf("  -p PORT      Telnet server port (default: 9091)\n");
    printf("  -d SECONDS   Test duration in seconds (default: 30)\n");
    printf("  -v           Verbose output\n");
    printf("  --help       Show this help message\n");
    printf("\nTest Description:\n");
    printf("  1. Connect to telnet server\n");
    printf("  2. Receive text from server for specified duration\n");
    printf("  3. Send test strings: \"abcd\", \"한글\", \"こんにちは。\"\n");
    printf("  4. Each string sent at 3-second intervals\n");
    printf("  5. Verify echo responses from server\n");
}

int main(int argc, char *argv[])
{
    standalone_test_t test;
    int opt;
    int result = SUCCESS;

    /* Initialize test context */
    memset(&test, 0, sizeof(test));
    strcpy(test.host, "127.0.0.1");
    test.port = 9091;
    test.test_duration = 30;
    test.verbose = false;

    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "h:p:d:v")) != -1) {
        switch (opt) {
            case 'h':
                strncpy(test.host, optarg, sizeof(test.host) - 1);
                test.host[sizeof(test.host) - 1] = '\0';
                break;
            case 'p':
                test.port = atoi(optarg);
                if (test.port <= 0 || test.port > 65535) {
                    printf("[ERROR] Invalid port number: %s\n", optarg);
                    return ERROR_INVALID_ARG;
                }
                break;
            case 'd':
                test.test_duration = atoi(optarg);
                if (test.test_duration <= 0) {
                    printf("[ERROR] Invalid duration: %s\n", optarg);
                    return ERROR_INVALID_ARG;
                }
                break;
            case 'v':
                test.verbose = true;
                break;
            default:
                print_usage(argv[0]);
                return ERROR_INVALID_ARG;
        }
    }

    /* Setup signals */
    setup_signals();

    printf("ModemBridge Level 2 Standalone Telnet Test v1.0.0\n");
    printf("================================================\n\n");

    /* Run the test */
    result = run_standalone_test(&test);

    printf("\nTest completed with result: %s\n",
           (result == SUCCESS) ? "SUCCESS" : error_to_string(result));

    return (result == SUCCESS) ? 0 : 1;
}