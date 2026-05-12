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
 * The OTA token is read from settings_get()->ota_token at request time, so
 * it can be changed via POST /api/settings without reflashing. An empty token
 * string means open access. Comparison is constant-time (XOR accumulator) to
 * prevent timing oracle attacks. This is not cryptographic security — it is a
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
#include "settings_mgr.h"
#include "config.h"

#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota_server";

#define OTA_CHUNK_SIZE   4096   /* bytes read per httpd_req_recv() call */

/* ── Graceful shutdown hook ─────────────────────────────────────────────────
 * Registered via ota_server_set_shutdown_cb(). Called before esp_restart()
 * so the caller can close the HTTP server and send WS close frames, giving
 * the browser time to display "updating…" instead of a hung connection. */
static ota_shutdown_cb_t s_shutdown_cb   = NULL;
static void             *s_shutdown_ctx  = NULL;

void ota_server_set_shutdown_cb(ota_shutdown_cb_t cb, void *ctx)
{
    s_shutdown_cb  = cb;
    s_shutdown_ctx = ctx;
}

/* Invoke the shutdown hook (if registered) then restart. */
static void graceful_restart(void)
{
    if (s_shutdown_cb) {
        s_shutdown_cb(s_shutdown_ctx);
        /* Allow the callback time to flush WS close frames and drain TCP. */
        vTaskDelay(pdMS_TO_TICKS(800));
    } else {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_restart();
}

/* ── SHA-256 hex helpers ────────────────────────────────────────────────────
 * Reads the X-OTA-SHA256 request header (64 hex chars = 32 bytes).
 * Returns false if the header is absent or malformed. */
static bool parse_sha256_header(httpd_req_t *req, uint8_t out[32])
{
    char hex[65] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256",
                                    hex, sizeof(hex)) != ESP_OK) {
        return false;   /* header absent — skip verification */
    }
    if (strlen(hex) != 64) {
        ESP_LOGW(TAG, "X-OTA-SHA256 wrong length (%u)", (unsigned)strlen(hex));
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned byte;
        if (sscanf(hex + 2 * i, "%02x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

static bool sha256_matches(const uint8_t computed[32], const uint8_t expected[32])
{
    /* Constant-time compare — same rationale as the token check. */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= computed[i] ^ expected[i];
    return diff == 0;
}

/* ── Auth check ─────────────────────────────────────────────────────────────
 * Returns true if auth passes (token matches or no token configured).
 * Sends 401 and returns false on failure.
 *
 * Token is read from settings_get()->ota_token at request time so it can
 * be changed via /api/settings without reflashing.
 *
 * Comparison is constant-time (byte-by-byte XOR accumulator) to prevent
 * timing oracle attacks — strcmp() short-circuits on the first mismatch. */
static bool auth_ok(httpd_req_t *req)
{
    const char *token_cfg = settings_get()->ota_token;
    if (token_cfg[0] == '\0') return true;   /* no token configured → open */

    char token[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token",
                                    token, sizeof(token)) != ESP_OK) {
        goto reject;
    }

    /* Constant-time comparison: lengths must match first (avoids padding
     * the shorter string, which would itself be a timing side-channel). */
    size_t cfg_len = strlen(token_cfg);
    size_t tok_len = strlen(token);
    if (cfg_len != tok_len) goto reject;

    uint8_t diff = 0;
    for (size_t i = 0; i < cfg_len; i++)
        diff |= (uint8_t)token_cfg[i] ^ (uint8_t)token[i];
    if (diff != 0) goto reject;

    return true;

reject:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "Bad or missing X-OTA-Token");
    ESP_LOGW(TAG, "OTA request rejected — bad token");
    return false;
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

    /* Optional SHA-256 verification — absent header skips the check. */
    uint8_t expected_sha[32];
    bool    verify_sha = parse_sha256_header(req, expected_sha);
    if (verify_sha)
        ESP_LOGI(TAG, "SHA-256 verification enabled");

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

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0 /* SHA-256, not SHA-224 */);

    while (received < content_len) {
        int to_read = content_len - received;
        if (to_read > OTA_CHUNK_SIZE) to_read = OTA_CHUNK_SIZE;

        int n = httpd_req_recv(req, buf, to_read);
        if (n <= 0) {
            ESP_LOGE(TAG, "recv error at offset %d", received);
            failed = true;
            break;
        }

        mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buf, n);

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        received += n;
    }

    /* Verify checksum before committing — abort if mismatch. */
    if (!failed && verify_sha) {
        uint8_t computed[32];
        mbedtls_sha256_finish(&sha_ctx, computed);
        mbedtls_sha256_free(&sha_ctx);

        if (!sha256_matches(computed, expected_sha)) {
            ESP_LOGE(TAG, "SHA-256 mismatch — aborting OTA");
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "SHA-256 checksum mismatch — upload corrupted");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "SHA-256 verified OK");
    } else {
        mbedtls_sha256_free(&sha_ctx);
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

    /* Graceful shutdown: send WS close frames, then restart. */
    graceful_restart();
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

    /* Optional SHA-256 verification */
    uint8_t expected_sha[32];
    bool    verify_sha = parse_sha256_header(req, expected_sha);

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

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
        mbedtls_sha256_free(&sha_ctx);
    }

    while (!failed && received < content_len) {
        int to_read = content_len - received;
        if (to_read > OTA_CHUNK_SIZE) to_read = OTA_CHUNK_SIZE;

        int n = httpd_req_recv(req, buf, to_read);
        if (n <= 0) { failed = true; break; }

        mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buf, n);

        err = esp_partition_write(fs_part, received, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition write failed at %d: %s",
                     received, esp_err_to_name(err));
            failed = true;
            break;
        }
        received += n;
    }

    /* Verify checksum if header was provided */
    if (!failed && verify_sha) {
        uint8_t computed[32];
        mbedtls_sha256_finish(&sha_ctx, computed);
        mbedtls_sha256_free(&sha_ctx);
        if (!sha256_matches(computed, expected_sha)) {
            ESP_LOGE(TAG, "Filesystem SHA-256 mismatch — partition may be corrupt");
            failed = true;
        } else {
            ESP_LOGI(TAG, "Filesystem SHA-256 verified OK");
        }
    } else if (!failed) {
        mbedtls_sha256_free(&sha_ctx);
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
