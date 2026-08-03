#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 全局配置结构体
 *
 * 与 SD 卡 /config.json 一一对应。加载失败时使用默认值。
 */
typedef struct {
    uint16_t interval_minutes;   ///< 拍照间隔（分钟）
    char     resolution[16];     ///< 分辨率，如 "1600x1200"
    int      quality;            ///< JPEG 质量（esp_camera 数值越小质量越高）
    bool     upload_enabled;     ///< 是否启用 WiFi 回传
    char     wifi_ssid[64];      ///< WiFi SSID
    char     wifi_password[64];  ///< WiFi 密码
    char     server_url[128];    ///< 上传服务器 URL
} config_t;

/**
 * @brief 从 SD 卡加载配置 (/sdcard/config.json)
 *
 * 文件不存在或 JSON 损坏时填充内置默认值，保证系统可启动。
 * 每个字段独立 try-parse，缺失或类型不符则保留默认值。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 文件不存在或 JSON 损坏（已填充默认值）
 */
esp_err_t config_mgr_load(void);

/**
 * @brief 保存当前配置到 SD 卡 /sdcard/config.json
 *
 * @return ESP_OK 成功
 */
esp_err_t config_mgr_save(void);

/**
 * @brief 获取配置指针（只读使用）
 *
 * @return const config_t* 配置结构体指针；未加载时返回 NULL，
 *         调用方可改用 config_mgr_get_default() 获取默认配置
 */
const config_t *config_mgr_get(void);

/**
 * @brief 用 memcpy 覆盖当前配置（不自动保存）
 *
 * 供配置门户等场景在内存中修改配置后调用，调用方决定是否 config_mgr_save()。
 *
 * @param new_cfg 新配置指针
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空
 */
esp_err_t config_mgr_set(const config_t *new_cfg);

/**
 * @brief 获取内置默认配置指针（用于配置门户展示或加载失败兜底）
 *
 * @return const config_t* 默认配置指针（始终非 NULL）
 */
const config_t *config_mgr_get_default(void);

#ifdef __cplusplus
}
#endif
