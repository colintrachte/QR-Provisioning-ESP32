/**
 * ota_server.c — OTA firmware and filesystem update endpoints.
 *
 * Streaming upload model
 * ──────────────────────
 * Both handlers read the POST body in OTA_CHUNK_SIZE chunks directly into
 * the flash write buffer. Peak RAM overhead is one chunk — independent of
 * firmware size. A 3 MB firmware over WiFi at ~500 KB/s takes ~6 s.
 *
 * Auth
 * ────
 * If OTA_AUTH_TOKEN is non-empty the handler checks the X-OTA-Token request
 * header before touching flash. A missing or wrong token returns 401 and
 * aborts immediately. This is not cryptographic security — it is a simple
 * guard against accidental or opportunistic flashing on a LAN.
 *
 * Filesystem update
 * ─────────────────
 * /ota/filesystem writes directly to the "storage" partition using
 * esp_partition_write(). The LittleFS volume is unmounted before writing
 * and remounted after — no reboot required. All open file handles must be
 * closed before calling this endpoint; the WebSocket server continues
 * serving from RAM while the write is in progress.
 */

#include "ota_server.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_littlefs.h"

static const char *TAG = "ota_server";

#define OTA_CHUNK_SIZE   4096   /* bytes read per httpd_req_recv() call */

/* ── Auth check ─────────────────────────────────────────────────────────────
 * Returns true if auth passes (token matches or no token configured).
 * Sends 401 and returns false on failure. */
static bool auth_ok(httpd_req_t *req)
{
#ifndef OTA_AUTH_TOKEN
    return true;
#else
    if (sizeof(OTA_AUTH_TOKEN) <= 1) return true;   /* empty string → open */

    char token[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token",
                                    token, sizeof(token)) != ESP_OK ||
        strcmp(token, OTA_AUTH_TOKEN) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Bad or missing X-OTA-Token");
        ESP_LOGW(TAG, "OTA request rejected — bad token");
        return false;
    }
    return true;
#endif
}

/* ── Firmware OTA handler ────────────────────────────────────────────────────
 * POST /ota/firmware
 *
 * Flashes the upload to the inactive OTA partition, marks it as the next
 * boot target, and reboots. On any error the active partition is unchanged. */
static esp_err_t handle_ota_firmware(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    int content_len = req->content_len;
    ESP_LOGI(TAG, "Firmware OTA start (%d bytes)", content_len);

    const esp_partition_t *update_part =
        esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA partition found — check partitions.csv");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No OTA partition");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx",
             update_part->label, update_part->address);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES,
                                  &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "OTA begin failed");
        return ESP_OK;
    }

    static char buf[OTA_CHUNK_SIZE];
    int received = 0;
    bool failed  = false;

    while (received < content_len) {
        int to_read = content_len - received;
        if (to_read > OTA_CHUNK_SIZE) to_read = OTA_CHUNK_SIZE;

        int n = httpd_req_recv(req, buf, to_read);
        if (n <= 0) {
            ESP_LOGE(TAG, "recv error at offset %d", received);
            failed = true;
            break;
        }

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        received += n;
    }

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Upload failed");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "OTA end failed (bad image?)");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Set boot partition failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Firmware OTA complete (%d bytes) — rebooting", received);
    httpd_resp_sendstr(req, "OK — rebooting");

    /* Small delay so the response reaches the browser before the reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── Filesystem OTA handler ─────────────────────────────────────────────────
 * POST /ota/filesystem
 *
 * Writes directly to the LittleFS storage partition. Unmounts the volume
 * first, writes in 4 kB blocks aligned to the flash erase unit, then
 * remounts. The firmware keeps running — no reboot needed. */
static esp_err_t handle_ota_filesystem(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    int content_len = req->content_len;
    ESP_LOGI(TAG, "Filesystem OTA start (%d bytes)", content_len);

    const esp_partition_t *fs_part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                 "storage");
    if (!fs_part) {
        ESP_LOGE(TAG, "storage partition not found");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "storage partition not found");
        return ESP_OK;
    }

    if (content_len > (int)fs_part->size) {
        ESP_LOGE(TAG, "Image (%d B) larger than partition (%lu B)",
                 content_len, fs_part->size);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Image too large for storage partition");
        return ESP_OK;
    }

    /* Declare all locals before any early-exit path so the compiler can
     * verify initialization regardless of which branch is taken. */
    static char buf[OTA_CHUNK_SIZE];
    int      received = 0;
    bool     failed   = false;
    esp_err_t err;

    /* Unmount LittleFS before writing. All in-flight file operations on
     * other tasks must have completed — the WS handler doesn't open files
     * so this is safe during normal operation. */
    esp_vfs_littlefs_unregister("storage");
    ESP_LOGI(TAG, "LittleFS unmounted");

    /* Erase the entire partition before writing. */
    err = esp_partition_erase_range(fs_part, 0, fs_part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        failed = true;   /* skip write loop, fall through to remount */
    }

    while (!failed && received < content_len) {
        int to_read = content_len - received;
        if (to_read > OTA_CHUNK_SIZE) to_read = OTA_CHUNK_SIZE;

        int n = httpd_req_recv(req, buf, to_read);
        if (n <= 0) { failed = true; break; }

        err = esp_partition_write(fs_part, received, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition write failed at %d: %s",
                     received, esp_err_to_name(err));
            failed = true;
            break;
        }
        received += n;
    }

    /* Always remount — even after a failed write, the filesystem must be
     * available so the rest of the firmware keeps working. */
    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = "/littlefs",
        .partition_label        = "storage",
        .format_if_mount_failed = false,
    };
    esp_err_t mount_err = esp_vfs_littlefs_register(&cfg);
    if (mount_err != ESP_OK)
        ESP_LOGE(TAG, "LittleFS remount failed: %s", esp_err_to_name(mount_err));
    else
        ESP_LOGI(TAG, "LittleFS remounted");

    if (failed) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Filesystem upload failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Filesystem OTA complete (%d bytes)", received);
    httpd_resp_sendstr(req, "OK — filesystem updated");
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

esp_err_t ota_server_register(httpd_handle_t server)
{
    static const httpd_uri_t fw_route = {
        .uri     = "/ota/firmware",
        .method  = HTTP_POST,
        .handler = handle_ota_firmware,
    };
    static const httpd_uri_t fs_route = {
        .uri     = "/ota/filesystem",
        .method  = HTTP_POST,
        .handler = handle_ota_filesystem,
    };

    esp_err_t err;
    err = httpd_register_uri_handler(server, &fw_route);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(server, &fs_route);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "OTA endpoints registered: POST /ota/firmware, POST /ota/filesystem");
    return ESP_OK;
}

void ota_server_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Firmware marked valid — rollback cancelled");
    else if (err != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGW(TAG, "mark_valid: %s", esp_err_to_name(err));
    /* ESP_ERR_NOT_SUPPORTED = rollback not enabled — silent, expected */
}
