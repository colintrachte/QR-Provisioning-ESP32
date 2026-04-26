/**
 * vfs_mount.c — Shared LittleFS mount helper.
 *
 * Both portal.c and app_server.c need LittleFS mounted.
 * This module ensures the mount happens exactly once and is robust
 * against ESP_ERR_INVALID_STATE (already mounted).
 */

#include "vfs_mount.h"
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "vfs_mount";

static bool s_mounted = false;

esp_err_t vfs_mount_littlefs(void)
{
    if (s_mounted) {
        ESP_LOGD(TAG, "LittleFS already mounted");
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = "/littlefs",
        .partition_label        = "storage",
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "LittleFS mounted at /littlefs");
    } else {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
    }
    return err;
}
