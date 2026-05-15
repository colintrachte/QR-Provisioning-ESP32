/**
 * portal.c — Thin provisioning coordinator.
 *
 * All HTTP serving, DNS hijacking, and captive-portal probes are now owned
 * by app_server.c, which runs on port 80 from boot.  portal.c has one job:
 * block wifi_manager's mgr_task until the user submits credentials.
 *
 * Lifecycle
 * ─────────
 *   portal_start()  — registers a credential-received callback with
 *                     app_server, asks app_server to enter provisioning
 *                     mode (registers setup routes + DNS), then blocks
 *                     on an EventGroup bit.
 *
 *   portal_stop()   — asks app_server to leave provisioning mode
 *                     (unregisters setup routes + stops DNS).
 *
 *   portal_get_credentials() — copies the SSID/pass captured by
 *                     app_server's /api/connect handler.
 *
 * The credential storage (s_ssid / s_pass) lives here so that portal.h's
 * public interface is unchanged from wifi_manager's perspective.
 */

#include "portal.h"
#include "app_server.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "portal";

/* ── Credential storage ─────────────────────────────────────────────────── */
static char s_ssid[33] = {0};
static char s_pass[65] = {0};

/* ── Synchronisation ────────────────────────────────────────────────────── */
#define CREDS_RECEIVED_BIT  BIT0
static EventGroupHandle_t s_event_group = NULL;

/* ── Callback invoked by app_server's /api/connect handler ─────────────── */
static void on_credentials(const char *ssid, const char *pass)
{
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, pass, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';

    ESP_LOGI(TAG, "Credentials received: SSID='%s'", s_ssid);

    if (s_event_group)
        xEventGroupSetBits(s_event_group, CREDS_RECEIVED_BIT);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void portal_start(const char *ap_ssid)
{
    (void)ap_ssid;   /* SSID used by app_server directly via wifi_manager API */

    s_ssid[0] = '\0';
    s_pass[0] = '\0';

    s_event_group = xEventGroupCreate();

    /* Hand our credential callback to app_server and flip it into
     * provisioning mode: registers setup-page routes, captive-portal
     * probe handlers, and starts the DNS hijack task. */
    app_server_enter_provisioning_mode(on_credentials);

    ESP_LOGI(TAG, "Portal active — waiting for credentials");
    xEventGroupWaitBits(s_event_group, CREDS_RECEIVED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Credentials received — portal unblocking");
}

void portal_stop(void)
{
    /* Ask app_server to unregister provisioning routes and stop DNS.
     * Control-page routes and WebSocket remain active throughout. */
    app_server_exit_provisioning_mode();

    if (s_event_group) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    ESP_LOGI(TAG, "Portal stopped");
}

void portal_get_credentials(char *out_ssid, char *out_pass)
{
    strncpy(out_ssid, s_ssid, 33); out_ssid[32] = '\0';
    strncpy(out_pass, s_pass, 65); out_pass[64] = '\0';
    ESP_LOGI(TAG, "portal_get_credentials: SSID='%s'", out_ssid);
}
