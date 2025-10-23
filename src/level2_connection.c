/*
 * level2_connection.c - Level 2 (Telnet) Connection Management Implementation
 *
 * This file implements functions for managing telnet connections
 * including connection establishment, disconnection, and echo mode synchronization.
 */

#ifdef ENABLE_LEVEL2

#include "bridge.h"
#include "level2_connection.h"
#include "telnet.h"
#include "modem.h"
#include "common.h"
#include <string.h>

/* Forward declaration for internal function */
static int bridge_reinitialize_modem(bridge_ctx_t *ctx);

/**
 * Handle telnet connection establishment (Level 2 only)
 */
int bridge_handle_telnet_connect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Telnet connection established");

    ctx->state = STATE_CONNECTED;

    return SUCCESS;
}

/**
 * Handle telnet disconnection (Level 2 only)
 */
int bridge_handle_telnet_disconnect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Telnet disconnected - initiating modem reinitialization");

    /* Hang up modem first */
    if (modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
        modem_send_no_carrier(&ctx->modem);
    }

    /* Reinitialize modem to initial state */
    int ret = bridge_reinitialize_modem(ctx);
    if (ret != SUCCESS) {
        MB_LOG_WARNING("Failed to reinitialize modem after telnet disconnect: %d", ret);
        /* Continue anyway - don't fail the disconnect handling */
    }

    ctx->state = STATE_IDLE;

    MB_LOG_INFO("Modem returned to initial state - ready for new connection");

    return SUCCESS;
}

/**
 * Handle modem disconnection (Level 2 only)
 */
int bridge_handle_modem_disconnect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Modem disconnected - cleaning up connection");

    /* Disconnect telnet if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }

    /* Send NO CARRIER */
    modem_send_no_carrier(&ctx->modem);

    /* Reinitialize modem to initial state */
    int ret = bridge_reinitialize_modem(ctx);
    if (ret != SUCCESS) {
        MB_LOG_WARNING("Failed to reinitialize modem after modem disconnect: %d", ret);
        /* Continue anyway - don't fail the disconnect handling */
    }

    ctx->state = STATE_IDLE;

    MB_LOG_INFO("Modem disconnected and cleaned up");

    return SUCCESS;
}

/**
 * Synchronize echo mode between telnet and modem (Level 2 only)
 *
 * This function ensures proper echo behavior to prevent double echo.
 * If the telnet server will echo, we disable modem local echo.
 */
void bridge_sync_echo_mode(bridge_ctx_t *ctx)
{
    if (ctx == NULL || !telnet_is_connected(&ctx->telnet)) {
        return;
    }

    /* If server will echo, disable modem local echo to prevent double echo */
    if (ctx->telnet.remote_options[TELOPT_ECHO]) {
        if (ctx->modem.settings.echo) {
            MB_LOG_INFO("Server WILL ECHO - disabling modem local echo to prevent double echo");
            ctx->modem.settings.echo = false;
        }
    }
    /* If server won't echo, keep modem echo setting (from ATE command) */
    else {
        MB_LOG_INFO("Server WONT ECHO - using modem echo setting (ATE command)");
    }
}

/**
 * Reinitialize modem to initial state
 * This function is called after connection termination to reset modem
 * to the same state as program startup
 */
static int bridge_reinitialize_modem(bridge_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->modem_ready) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Reinitializing modem to initial state");

    /* First, ensure modem is in command mode */
    if (modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
    }

    /* Reset modem to factory defaults (ATZ) */
    modem_init(&ctx->modem, &ctx->serial);

    /* Send initialization command if configured */
    if (ctx->config->modem_init_command && strlen(ctx->config->modem_init_command) > 0) {
        MB_LOG_INFO("Sending modem init command: %s", ctx->config->modem_init_command);
        char cmd_buf[1056];  /* MAX_CONFIG_LINE_LENGTH + extra space for \r */
        snprintf(cmd_buf, sizeof(cmd_buf), "%s\r", ctx->config->modem_init_command);
        serial_write(&ctx->serial, (unsigned char*)cmd_buf, strlen(cmd_buf));
        usleep(100000);  /* 100ms delay for init command */
    }

    /* Send MODEM_COMMAND if configured */
    if (ctx->config->modem_command && strlen(ctx->config->modem_command) > 0) {
        MB_LOG_INFO("Sending MODEM_COMMAND for reinitialization: %s", ctx->config->modem_command);
        char cmd_buf[1056];  /* MAX_CONFIG_LINE_LENGTH + extra space for \r */
        snprintf(cmd_buf, sizeof(cmd_buf), "%s\r", ctx->config->modem_command);
        serial_write(&ctx->serial, (unsigned char*)cmd_buf, strlen(cmd_buf));
        usleep(100000);  /* 100ms delay for modem command */
    }

    /* Restore auto-answer if configured */
    if (ctx->config->modem_autoanswer_mode > 0) {
        /* Build auto-answer command: ATS0=n where n is rings */
        MB_LOG_INFO("Re-enabling auto-answer: ATS0=%d", ctx->config->modem_autoanswer_mode);
        char cmd_buf[1056];  /* MAX_CONFIG_LINE_LENGTH + extra space for \r */
        snprintf(cmd_buf, sizeof(cmd_buf), "ATS0=%d\r", ctx->config->modem_autoanswer_mode);
        serial_write(&ctx->serial, (unsigned char*)cmd_buf, strlen(cmd_buf));
        usleep(100000);  /* 100ms delay */
    }

    MB_LOG_INFO("Modem reinitialization complete");
    return SUCCESS;
}

#endif /* ENABLE_LEVEL2 */