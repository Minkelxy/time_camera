#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi 上传模块
 *
 * 配置 STA 模式 SSID/密码，创建默认 netif 与事件循环（若尚未创建）。
 *
 * @param ssid     WiFi SSID
 * @param password WiFi 密码
 * @return ESP_OK 成功
 */
esp_err_t wifi_upload_init(const char *ssid, const char *password);

/**
 * @brief 连接 WiFi（阻塞直到获取 IP 或超时）
 *
 * @return ESP_OK 成功
 */
esp_err_t wifi_upload_connect(void);

/**
 * @brief 断开 WiFi
 *
 * @return ESP_OK 成功
 */
esp_err_t wifi_upload_disconnect(void);

/**
 * @brief 通过 HTTP POST 上传照片文件
 *
 * @param file_path  本地文件路径（如 /sdcard/photos/xxx.jpg）
 * @param server_url 服务器接收 URL
 * @return ESP_OK 成功
 */
esp_err_t wifi_upload_send_photo(const char *file_path, const char *server_url);

/**
 * @brief 反初始化，释放 WiFi 资源
 *
 * @return ESP_OK 成功
 */
esp_err_t wifi_upload_deinit(void);

#ifdef __cplusplus
}
#endif
