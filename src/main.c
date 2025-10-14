/*
 * main.c - Main entry point for ModemBridge
 */

#include "common.h"
#include "config.h"
#include "serial.h"
#include "modem.h"
#include "telnet.h"
#include "bridge.h"
#include <getopt.h>

/* Signal handler */
static void signal_handler(int signo)
{
    if (signo == SIGTERM || signo == SIGINT) {
        MB_LOG_INFO("Received signal %d, shutting down...", signo);
        g_running = 0;
    } else if (signo == SIGHUP) {
        MB_LOG_INFO("Received SIGHUP, reloading configuration...");
        g_reload_config = 1;
    }
}

/* Setup signal handlers */
static int setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        MB_LOG_ERROR("Failed to setup SIGTERM handler: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        MB_LOG_ERROR("Failed to setup SIGINT handler: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        MB_LOG_ERROR("Failed to setup SIGHUP handler: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    return SUCCESS;
}

/* Print usage information */
static void print_usage(const char *prog_name)
{
    printf("ModemBridge v%s - Dialup Modem to Telnet Bridge\n", MODEMBRIDGE_VERSION);
    printf("\n");
    printf("Usage: %s [options]\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
    printf("  -d, --daemon         Run as daemon\n");
    printf("  -p, --pid-file FILE  PID file (default: %s)\n", DEFAULT_PID_FILE);
    printf("  -v, --verbose        Verbose logging\n");
    printf("  -h, --help           Show this help message\n");
    printf("  -V, --version        Show version information\n");
    printf("\n");
}

/* Main function */
int main(int argc, char *argv[])
{
    config_t config;
    char config_file[SMALL_BUFFER_SIZE] = {0};
    bool daemon_mode = false;
    bool verbose = false;
    int ret = SUCCESS;

    /* Parse command line arguments */
    static struct option long_options[] = {
        {"config",   required_argument, 0, 'c'},
        {"daemon",   no_argument,       0, 'd'},
        {"pid-file", required_argument, 0, 'p'},
        {"verbose",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {"version",  no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    while ((c = getopt_long(argc, argv, "c:dp:vhV", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                SAFE_STRNCPY(config_file, optarg, sizeof(config_file));
                break;
            case 'd':
                daemon_mode = true;
                break;
            case 'p':
                /* PID file option - handled later */
                break;
            case 'v':
                verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return SUCCESS;
            case 'V':
                printf("ModemBridge v%s\n", MODEMBRIDGE_VERSION);
                return SUCCESS;
            default:
                print_usage(argv[0]);
                return ERROR_INVALID_ARG;
        }
    }

    /* Initialize syslog */
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_DAEMON);

    if (verbose) {
        setlogmask(LOG_UPTO(LOG_DEBUG));
    } else {
        setlogmask(LOG_UPTO(LOG_INFO));
    }

    MB_LOG_INFO("=== ModemBridge v%s starting ===", MODEMBRIDGE_VERSION);

    /* Load configuration */
    config_init(&config);

    if (strlen(config_file) == 0) {
        SAFE_STRNCPY(config_file, DEFAULT_CONFIG_FILE, sizeof(config_file));
    }

    if (config_load(&config, config_file) != SUCCESS) {
        MB_LOG_ERROR("Failed to load configuration");
        ret = ERROR_CONFIG;
        goto cleanup;
    }

    if (config_validate(&config) != SUCCESS) {
        MB_LOG_ERROR("Configuration validation failed");
        ret = ERROR_CONFIG;
        goto cleanup;
    }

    config_print(&config);

    /* Setup signal handlers */
    if (setup_signals() != SUCCESS) {
        MB_LOG_ERROR("Failed to setup signal handlers");
        ret = ERROR_GENERAL;
        goto cleanup;
    }

    /* Daemonize if requested */
    if (daemon_mode) {
        MB_LOG_INFO("Entering daemon mode...");
        if (daemonize() != SUCCESS) {
            MB_LOG_ERROR("Failed to daemonize");
            ret = ERROR_GENERAL;
            goto cleanup;
        }
    }

    /* Write PID file */
    if (write_pid_file(config.pid_file) != SUCCESS) {
        MB_LOG_WARNING("Failed to write PID file");
        /* Continue anyway */
    }

    /* Initialize and run bridge */
    bridge_ctx_t bridge;
    bridge_init(&bridge, &config);

    if (bridge_start(&bridge) != SUCCESS) {
        MB_LOG_ERROR("Failed to start bridge");
        ret = ERROR_GENERAL;
        goto cleanup_bridge;
    }

    /* Main loop */
    while (g_running) {
        if (bridge_run(&bridge) != SUCCESS) {
            MB_LOG_ERROR("Bridge error, exiting...");
            break;
        }

        if (g_reload_config) {
            MB_LOG_INFO("Reloading configuration...");
            config_load(&config, config_file);
            config_validate(&config);
            config_print(&config);
            g_reload_config = 0;
        }
    }

cleanup_bridge:
    bridge_stop(&bridge);

cleanup:
    /* Cleanup */
    config_free(&config);
    remove_pid_file(config.pid_file);

    MB_LOG_INFO("=== ModemBridge shutdown complete ===");
    closelog();

    return ret;
}
