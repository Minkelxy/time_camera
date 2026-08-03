#include "config_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sd_storage.h"

static const char *TAG = "config_mgr";

#define CONFIG_PATH "/sdcard/config.json"

/* 内置默认配置（SD 卡加载失败时使用，亦供 config_portal 展示） */
static const config_t s_default_config = {
    .interval_minutes = 5,
    .resolution       = "1600x1200",
    .quality          = 10,        /* 越小质量越高，10 为默认 */
    .upload_enabled   = false,
    .wifi_ssid        = "",
    .wifi_password    = "",
    .server_url       = "",
};

/* 当前生效配置；load 前为零值，s_loaded 标记是否成功从 SD 卡加载 */
static config_t s_config;
static bool s_loaded = false;

/**
 * @brief 将 s_config 重置为内置默认值
 */
static void config_apply_defaults(void)
{
    memcpy(&s_config, &s_default_config, sizeof(config_t));
}

/**
 * @brief 安全拷贝字符串字段，保证 NUL 终止
 */
static void copy_string_field(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0 || src == NULL) {
        return;
    }
    size_t len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

esp_err_t config_mgr_load(void)
{
    /* 先应用默认值，确保任何分支下 s_config 都有可用值 */
    config_apply_defaults();
    s_loaded = false;

    /* 1. 从 SD 卡读取配置文件 */
    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t ret = sd_storage_read_file(CONFIG_PATH, &buf, &len);
    if (ret != ESP_OK || buf == NULL || len == 0) {
        ESP_LOGW(TAG, "配置文件读取失败 (%s)，使用默认配置",
                 esp_err_to_name(ret));
        /* read_file 失败时 *out_buf 为 NULL，无需 free；正常返回但 len=0 时 buf 可能为非空 */
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }

    /* 2. cJSON 解析。read_file 返回的缓冲无 NUL 终止，使用 ParseWithLength */
    cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败，使用默认配置");
        return ESP_ERR_NOT_FOUND;
    }

    /* 3. 逐字段 try-parse，缺失或类型不符则保留默认值 */
    cJSON *item = NULL;

    item = cJSON_GetObjectItem(root, "interval_minutes");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v > 0 && v <= 1440) {   /* 1 分钟 ~ 24 小时 */
            s_config.interval_minutes = (uint16_t)v;
        }
    }

    item = cJSON_GetObjectItem(root, "resolution");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        copy_string_field(s_config.resolution, sizeof(s_config.resolution),
                          item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "quality");
    if (cJSON_IsNumber(item)) {
        int q = item->valueint;
        if (q >= 4 && q <= 63) {    /* esp_camera JPEG 质量范围 */
            s_config.quality = q;
        }
    }

    item = cJSON_GetObjectItem(root, "upload_enabled");
    if (cJSON_IsBool(item)) {
        s_config.upload_enabled = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        copy_string_field(s_config.wifi_ssid, sizeof(s_config.wifi_ssid),
                          item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "wifi_password");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        copy_string_field(s_config.wifi_password, sizeof(s_config.wifi_password),
                          item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "server_url");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        copy_string_field(s_config.server_url, sizeof(s_config.server_url),
                          item->valuestring);
    }

    cJSON_Delete(root);
    s_loaded = true;

    ESP_LOGI(TAG, "配置加载成功：interval=%u, resolution=%s, quality=%d, upload=%d",
             (unsigned)s_config.interval_minutes, s_config.resolution,
             s_config.quality, (int)s_config.upload_enabled);
    return ESP_OK;
}

esp_err_t config_mgr_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateObject 失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "interval_minutes", s_config.interval_minutes);
    cJSON_AddStringToObject(root, "resolution", s_config.resolution);
    cJSON_AddNumberToObject(root, "quality", s_config.quality);
    cJSON_AddBoolToObject(root, "upload_enabled", s_config.upload_enabled);
    cJSON_AddStringToObject(root, "wifi_ssid", s_config.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", s_config.wifi_password);
    cJSON_AddStringToObject(root, "server_url", s_config.server_url);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted 失败");
        return ESP_FAIL;
    }

    esp_err_t ret = sd_storage_write_file(CONFIG_PATH, (const uint8_t *)str,
                                          strlen(str));
    cJSON_free(str);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置写入失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "配置已保存到 %s", CONFIG_PATH);
    }
    return ret;
}

const config_t *config_mgr_get(void)
{
    if (!s_loaded) {
        return NULL;
    }
    return &s_config;
}

esp_err_t config_mgr_set(const config_t *new_cfg)
{
    if (new_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_config, new_cfg, sizeof(config_t));
    s_loaded = true;
    return ESP_OK;
}

const config_t *config_mgr_get_default(void)
{
    return &s_default_config;
}
