/*
 * common.c - Common utility functions for ModemBridge
 */

#include "common.h"
#include <ctype.h>
#include <sys/file.h>

/* Global flags for signal handling */
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_reload_config = 0;

/**
 * Hexdump utility for debugging
 */
void hexdump(const char *label, const void *data, size_t len)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i, j;

    if (label) {
        MB_LOG_DEBUG("%s (%zu bytes):", label, len);
    }

    for (i = 0; i < len; i += 16) {
        char hex_buf[64] = {0};
        char ascii_buf[20] = {0};
        char line_buf[128] = {0};

        for (j = 0; j < 16 && (i + j) < len; j++) {
            sprintf(hex_buf + strlen(hex_buf), "%02x ", bytes[i + j]);
            ascii_buf[j] = isprint(bytes[i + j]) ? bytes[i + j] : '.';
        }

        sprintf(line_buf, "%08zx  %-48s  %s", i, hex_buf, ascii_buf);
        MB_LOG_DEBUG("%s", line_buf);
    }
}

/**
 * Trim leading and trailing whitespace from string
 */
char *trim_whitespace(char *str)
{
    char *end;

    if (str == NULL) {
        return NULL;
    }

    /* Trim leading space */
    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == 0) {
        return str;
    }

    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    /* Write new null terminator */
    *(end + 1) = '\0';

    return str;
}

/**
 * Daemonize the process
 */
int daemonize(void)
{
    pid_t pid;

    /* Fork parent process */
    pid = fork();
    if (pid < 0) {
        MB_LOG_ERROR("Failed to fork: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    /* Exit parent process */
    if (pid > 0) {
        exit(SUCCESS);
    }

    /* Child process becomes session leader */
    if (setsid() < 0) {
        MB_LOG_ERROR("Failed to create new session: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    /* Fork again to ensure daemon cannot acquire controlling terminal */
    pid = fork();
    if (pid < 0) {
        MB_LOG_ERROR("Failed to fork second time: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    if (pid > 0) {
        exit(SUCCESS);
    }

    /* Set file permissions */
    umask(0);

    /* Change working directory to root */
    if (chdir("/") < 0) {
        MB_LOG_ERROR("Failed to change directory to /: %s", strerror(errno));
        return ERROR_GENERAL;
    }

    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* Redirect standard file descriptors to /dev/null */
    int fd = open("/dev/null", O_RDWR);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) {
            close(fd);
        }
    }

    MB_LOG_INFO("Daemon started successfully");

    return SUCCESS;
}

/**
 * Write PID file
 */
int write_pid_file(const char *pid_file)
{
    FILE *fp;
    int fd;

    if (pid_file == NULL) {
        return ERROR_INVALID_ARG;
    }

    /* Open PID file */
    fp = fopen(pid_file, "w");
    if (fp == NULL) {
        MB_LOG_ERROR("Failed to open PID file %s: %s", pid_file, strerror(errno));
        return ERROR_IO;
    }

    fd = fileno(fp);

    /* Try to lock the file */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            MB_LOG_ERROR("Another instance is already running (PID file locked)");
        } else {
            MB_LOG_ERROR("Failed to lock PID file: %s", strerror(errno));
        }
        fclose(fp);
        return ERROR_GENERAL;
    }

    /* Write PID */
    fprintf(fp, "%d\n", getpid());
    fflush(fp);

    /* Keep file open to maintain lock */
    /* Note: In a real implementation, we should keep the FILE* handle */

    MB_LOG_INFO("PID file created: %s (PID: %d)", pid_file, getpid());

    return SUCCESS;
}

/**
 * Remove PID file
 */
int remove_pid_file(const char *pid_file)
{
    if (pid_file == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (unlink(pid_file) < 0) {
        if (errno != ENOENT) {
            MB_LOG_ERROR("Failed to remove PID file %s: %s", pid_file, strerror(errno));
            return ERROR_IO;
        }
    }

    MB_LOG_INFO("PID file removed: %s", pid_file);

    return SUCCESS;
}
