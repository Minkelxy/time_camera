#include "wifi_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "sd_storage.h"
#include "logger.h"

static const char *TAG = "wifi_upload";

/* ============================================================ */
/*                          常量定义                             */
/* ============================================================ */

/*! 连接成功事件位 */
#define WIFI_CONNECTED_BIT      BIT0
/*! 连接失败事件位 */
#define WIFI_FAIL_BIT           BIT1

/*! WiFi 连接超时（毫秒） */
#define WIFI_CONNECT_TIMEOUT_MS 15000
/*! HTTP 上传超时（毫秒） */
#define HTTP_TIMEOUT_MS         30000
/*! HTTP 失败重试次数（不含首次尝试） */
#define HTTP_MAX_RETRIES        2
/*! 单次上传照片大小上限（字节） */
#define MAX_PHOTO_SIZE          (500 * 1024)
/*! HTTP 头部 / 缓冲区大小 */
#define HTTP_BUFFER_SIZE        1024
/*! 重试间隔（毫秒） */
#define RETRY_DELAY_MS          1000

/* ============================================================ */
/*                          静态状态                             */
/* ============================================================ */

/*! 事件组句柄，init 时创建，deinit 时删除 */
static EventGroupHandle_t s_wifi_event_group = NULL;
/*! STA netif 句柄（用于 deinit 时释放） */
static esp_netif_t *s_sta_netif = NULL;
/*! 是否已初始化 */
static bool s_initialized = false;
/*! 事件回调是否已注册（deinit 时按需反注册） */
static bool s_event_registered = false;
/*! STA 断开后的重连计数（最多重连 1 次） */
static int s_retry_count = 0;
/*! 当前配置的 SSID */
static char s_ssid[64] = {0};
/*! 当前配置的密码 */
static char s_password[64] = {0};

/* ============================================================ */
/*                          事件回调                             */
/* ============================================================ */

/**
 * @brief WiFi / IP 事件回调
 *
 * - WIFI_EVENT_STA_DISCONNECTED：重连 1 次后仍失败则置 FAIL_BIT
 * - IP_EVENT_STA_GOT_IP：置 CONNECTED_BIT，复位重连计数
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < 1) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "STA 断开，尝试重连第 %d 次", s_retry_count);
        } else {
            ESP_LOGE(TAG, "STA 重连 1 次后仍失败，标记连接失败");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t wifi_upload_init(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 防重复初始化：仅更新 SSID/密码 */
    if (s_initialized) {
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
        if (password != NULL) {
            strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
        }
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
        strncpy(s_password, password ? password : "", sizeof(s_password) - 1);
        ESP_LOGI(TAG, "已初始化，仅更新 SSID=%s", ssid);
        return ESP_OK;
    }

    /* 1. 初始化 NVS（WiFi 校准数据需要；若已初始化则忽略） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需擦除后重新初始化");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 初始化底层网络接口（已初始化则忽略） */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 创建默认事件循环（已创建则忽略） */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "事件循环创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 创建默认 STA netif */
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* 5. 创建事件组 */
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "事件组创建失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    /* 6. 初始化 WiFi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(ret));
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ret;
    }

    /* 7. 注册事件回调 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    s_event_registered = true;

    /* 8. 配置 STA 模式与 SSID/密码 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password,
                sizeof(wifi_config.sta.password) - 1);
    }
    /* 启用快速连接扫描所有信道，避免 AP 信道不匹配时长时间连接失败 */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* 9. 存储方式设为 RAM（配置仅在内存中，避免污染 NVS） */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* 10. 缓存当前 SSID/密码 */
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_password, password ? password : "", sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi 上传模块初始化成功 SSID=%s", ssid);
    return ESP_OK;
}

esp_err_t wifi_upload_connect(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "未初始化，请先调用 wifi_upload_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 清除事件位，复位重连计数 */
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 等待连接成功或失败（15 秒超时） */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi 已连接 SSID=%s", s_ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi 连接超时（%d ms）", WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 单次 HTTP POST 上传尝试
 *
 * 使用 esp_http_client_open + esp_http_client_write 流式上传，
 * 避免内部额外拷贝。
 *
 * @param client     已初始化的 HTTP 客户端句柄
 * @param buf        待上传数据
 * @param len        数据长度
 * @return ESP_OK 成功（HTTP 200）
 */
static esp_err_t http_post_once(esp_http_client_handle_t client,
                                 const uint8_t *buf, size_t len)
{
    esp_err_t ret = ESP_FAIL;

    /* open：声明 Content-Length，进入写模式 */
    if (esp_http_client_open(client, (int)len) != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_open 失败");
        return ESP_FAIL;
    }

    /* 流式写入完整 body */
    int written = 0;
    size_t offset = 0;
    int zero_retry = 0;
    while (offset < len) {
        int w = esp_http_client_write(client, (const char *)(buf + offset),
                                      (int)(len - offset));
        if (w < 0) {
            ESP_LOGE(TAG, "esp_http_client_write 失败: %d (offset=%u)",
                     w, (unsigned)offset);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (w == 0) {
            /* 写入 0 字节通常表示底层超时，避免死循环：最多重试 3 次 */
            if (++zero_retry > 3) {
                ESP_LOGE(TAG, "HTTP write 连续 0 返回，放弃");
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        zero_retry = 0;
        written += w;
        offset += (size_t)w;
    }

    if ((size_t)written != len) {
        ESP_LOGE(TAG, "HTTP write 不完整: %d/%u", written, (unsigned)len);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 拉取响应头，获取状态码 */
    int fetch_ret = esp_http_client_fetch_headers(client);
    if (fetch_ret < 0) {
        ESP_LOGE(TAG, "esp_http_client_fetch_headers 失败: %d", fetch_ret);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP 响应码: %d", status);
    if (status == 200) {
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "HTTP 状态码非 200: %d", status);
        ret = ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t wifi_upload_send_photo(const char *file_path, const char *server_url)
{
    if (file_path == NULL || server_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. 读取整个照片文件到内存（受 MAX_PHOTO_SIZE 限制） */
    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t ret = sd_storage_read_file(file_path, &buf, &len);
    if (ret != ESP_OK || buf == NULL) {
        ESP_LOGE(TAG, "读取照片失败: %s (%s)", file_path, esp_err_to_name(ret));
        free(buf);
        return ret;
    }
    if (len == 0 || len > MAX_PHOTO_SIZE) {
        ESP_LOGE(TAG, "文件大小异常: %u bytes (上限 %u)",
                 (unsigned)len, (unsigned)MAX_PHOTO_SIZE);
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "上传照片 %s (%u bytes) -> %s",
             file_path, (unsigned)len, server_url);
    logger_log(LOG_LEVEL_INFO, "upload_start", "%s (%u bytes) -> %s",
               file_path, (unsigned)len, server_url);

    /* 2. 配置 HTTP 客户端 */
    esp_http_client_config_t config = {
        .url = server_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_BUFFER_SIZE,
        .disable_auto_redirect = false,
    };

    /* 3. 失败重试 HTTP_MAX_RETRIES 次（共 HTTP_MAX_RETRIES+1 次尝试） */
    esp_err_t upload_ret = ESP_FAIL;
    for (int attempt = 0; attempt <= HTTP_MAX_RETRIES; attempt++) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "HTTP 客户端初始化失败");
            if (attempt < HTTP_MAX_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                continue;
            }
            break;
        }
        esp_http_client_set_header(client, "Content-Type", "image/jpeg");

        upload_ret = http_post_once(client, buf, len);
        if (upload_ret == ESP_OK) {
            ESP_LOGI(TAG, "上传成功（尝试 %d）", attempt + 1);
            break;
        }

        ESP_LOGW(TAG, "上传失败（尝试 %d/%d）",
                 attempt + 1, HTTP_MAX_RETRIES + 1);
        if (attempt < HTTP_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
    }

    /* 4. 释放文件缓冲区 */
    free(buf);

    if (upload_ret == ESP_OK) {
        logger_log(LOG_LEVEL_INFO, "upload_done", "%s 上传成功", file_path);
    } else {
        logger_log(LOG_LEVEL_ERROR, "upload_failed", "%s 上传失败", file_path);
    }
    return upload_ret;
}

esp_err_t wifi_upload_disconnect(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* disconnect 可能返回 ESP_ERR_WIFI_NOT_CONNECT（未连接时），忽略 */
    esp_wifi_disconnect();
    /* stop 可能返回 ESP_ERR_WIFI_NOT_STARTED（已停止），忽略 */
    esp_wifi_stop();

    ESP_LOGI(TAG, "WiFi 已断开（保留 init 状态）");
    return ESP_OK;
}

esp_err_t wifi_upload_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* 反注册事件回调 */
    if (s_event_registered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifi_event_handler);
        s_event_registered = false;
    }

    /* 停止并释放 WiFi 驱动 */
    esp_wifi_stop();
    esp_wifi_deinit();

    /* 删除事件组 */
    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    /* 释放默认 STA netif */
    if (s_sta_netif != NULL) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    /* 复位状态 */
    s_initialized = false;
    s_retry_count = 0;
    s_ssid[0] = '\0';
    s_password[0] = '\0';

    ESP_LOGI(TAG, "WiFi 上传模块已反初始化");
    return ESP_OK;
}
