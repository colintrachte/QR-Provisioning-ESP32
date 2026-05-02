#include "utils_filesystem.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdbool.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "utils_filesystem";

/* ── LittleFS ───────────────────────────────────────────────────────────── */

/*
 * Both portal.c and app_server.c need LittleFS mounted.
 * This module ensures the mount happens exactly once and is robust
 * against ESP_ERR_INVALID_STATE (already mounted).
 */
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

void vfs_log_inventory(void)
{
    DIR *dir = opendir("/littlefs");
    if (!dir) {
        ESP_LOGW(TAG, "vfs_log_inventory: cannot open /littlefs — is LittleFS mounted?");
        return;
    }

    ESP_LOGI(TAG, "FS: --- LittleFS contents ---");

    struct dirent *entry;
    struct stat    st;
    size_t total = 0;
    int    count = 0;
    char   full[300];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;

        snprintf(full, sizeof(full), "/littlefs/%s", entry->d_name);

        size_t sz = 0;
        if (stat(full, &st) == 0) sz = (size_t)st.st_size;

        size_t dlen  = strlen(entry->d_name);
        bool   is_gz = (dlen > 3 &&
                        strcmp(entry->d_name + dlen - 3, ".gz") == 0);

        ESP_LOGI(TAG, "FS:   %-30s %6zu B%s",
                 entry->d_name, sz,
                 is_gz ? "  [gz]" : "");
        total += sz;
        count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "FS: --- %d files, %zu B total ---", count, total);
}
