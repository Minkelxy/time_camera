#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动配置门户
 *
 * 创建 SoftAP（SSID 如 "OutdoorCam-XXXX"）并启动 HTTP 服务器，
 * 用户连接 AP 后访问 192.168.4.1 进行 WiFi/服务器等参数配置。
 * 提交后调用 config_mgr_save() 持久化。
 *
 * @return ESP_OK 成功
 */
esp_err_t config_portal_start(void);

/**
 * @brief 停止配置门户，关闭 HTTP 服务器与 SoftAP
 *
 * @return ESP_OK 成功
 */
esp_err_t config_portal_stop(void);

#ifdef __cplusplus
}
#endif
