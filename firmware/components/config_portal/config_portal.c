#include "config_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_mgr.h"
#include "sd_storage.h"
#include "power_mgmt.h"
#include "logger.h"

static const char *TAG = "config_portal";

/* ============================================================ */
/*                          常量定义                             */
/* ============================================================ */

/*! SoftAP 默认密码（WPA2 至少 8 位） */
#define PORTAL_AP_PASSWORD       "12345678"
/*! SoftAP 信道 */
#define PORTAL_AP_CHANNEL        1
/*! SoftAP 最大连接数 */
#define PORTAL_AP_MAX_CONN       2
/*! 配置门户运行超时（30 分钟） */
#define PORTAL_RUN_TIMEOUT_MS    (30 * 60 * 1000)
/*! 阻塞轮询步长 */
#define PORTAL_POLL_STEP_MS      500
/*! 保存成功后延迟重启时间（毫秒） */
#define PORTAL_RESTART_DELAY_MS  3000
/*! POST body 最大长度 */
#define POST_BODY_MAX_LEN        1024
/*! HTML 页面缓冲区大小（含转义后的字段值，留足余量） */
#define HTML_PAGE_MAX_LEN        6144
/*! 表单字段值最大长度 */
#define FORM_FIELD_MAX_LEN       256

/* ============================================================ */
/*                          静态状态                             */
/* ============================================================ */

/*! HTTP 服务器句柄，供 stop 使用 */
static httpd_handle_t s_server = NULL;
/*! AP netif 句柄（用于 stop 时释放） */
static esp_netif_t *s_ap_netif = NULL;
/*! WiFi 是否已初始化（用于 stop 时判断是否需要 deinit） */
static bool s_wifi_inited = false;
/*! 是否已收到 /save 提交（用于在主循环中提前退出） */
static volatile bool s_save_received = false;

/* ============================================================ */
/*                       辅助函数：字符串处理                     */
/* ============================================================ */

/**
 * @brief URL 解码（application/x-www-form-urlencoded）
 *
 * 处理 %XX 十六进制转义与 '+' → 空格。
 *
 * @param dst       输出缓冲区
 * @param dst_size  输出缓冲区大小（含 '\0'）
 * @param src       输入字符串
 */
static void url_decode(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    size_t i = 0;
    while (*src != '\0' && i < dst_size - 1) {
        if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
            char hex[3] = {src[1], src[2], '\0'};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/**
 * @brief 从 application/x-www-form-urlencoded 的 body 中提取指定 key 的值
 *
 * body 形如：key1=val1&key2=val2&...
 *
 * @param body      POST body 字符串
 * @param key       要查找的字段名
 * @param out       输出缓冲区（已 URL 解码）
 * @param out_size  输出缓冲区大小
 * @return true 找到字段；false 未找到
 */
static bool form_get_value(const char *body, const char *key,
                           char *out, size_t out_size)
{
    if (body == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p != '\0') {
        /* 匹配 key= 模式 */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val_start = p + key_len + 1;
            const char *val_end = strchr(val_start, '&');
            size_t val_len = val_end ? (size_t)(val_end - val_start)
                                     : strlen(val_start);
            /* 拷贝到临时缓冲（保留原始 %XX） */
            char tmp[FORM_FIELD_MAX_LEN] = {0};
            size_t copy_len = val_len < sizeof(tmp) - 1 ? val_len : sizeof(tmp) - 1;
            memcpy(tmp, val_start, copy_len);
            tmp[copy_len] = '\0';
            /* URL 解码 */
            url_decode(out, out_size, tmp);
            return true;
        }
        /* 跳到下一个 '&' 之后 */
        const char *next = strchr(p, '&');
        if (next == NULL) {
            break;
        }
        p = next + 1;
    }
    return false;
}

/**
 * @brief HTML 转义，避免字段值破坏 HTML 结构
 *
 * 处理 < > & " 四种特殊字符。
 */
static void html_escape(char *out, size_t out_size, const char *in)
{
    if (out == NULL || out_size == 0 || in == NULL) {
        return;
    }
    size_t i = 0;
    while (*in != '\0' && i < out_size - 1) {
        switch (*in) {
        case '<':
            if (i + 4 < out_size) { strcpy(out + i, "&lt;");  i += 4; }
            else { out[i++] = '\0'; return; }
            break;
        case '>':
            if (i + 4 < out_size) { strcpy(out + i, "&gt;");  i += 4; }
            else { out[i++] = '\0'; return; }
            break;
        case '&':
            if (i + 5 < out_size) { strcpy(out + i, "&amp;"); i += 5; }
            else { out[i++] = '\0'; return; }
            break;
        case '"':
            if (i + 6 < out_size) { strcpy(out + i, "&quot;"); i += 6; }
            else { out[i++] = '\0'; return; }
            break;
        default:
            out[i++] = *in;
            break;
        }
        in++;
    }
    out[i] = '\0';
}

/**
 * @brief 生成 SoftAP SSID：OutdoorCam-XXXX（MAC 后 4 位十六进制）
 */
static void build_ap_ssid(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    /* 使用 esp_read_mac（无需 WiFi 已启动），获取 SoftAP MAC */
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_size, "OutdoorCam-%02X%02X", mac[4], mac[5]);
}

/* ============================================================ */
/*                       HTTP URI 处理器                         */
/* ============================================================ */

/**
 * @brief GET / - 返回配置 HTML 表单
 *
 * 表单字段均以 config_t 字段名命名，默认值从 config_mgr_get() 填充。
 */
static esp_err_t portal_get_root(httpd_req_t *req)
{
    /* config_mgr_get 在未加载时返回 NULL，回退到默认配置 */
    const config_t *cfg = config_mgr_get();
    if (cfg == NULL) {
        cfg = config_mgr_get_default();
    }

    /* 转义所有用于 HTML 属性值的字符串
     * 缓冲区按最坏情况分配（每个字符最多展开为 6 字节 &quot;） */
    char ssid_esc[64 * 6 + 1]  = {0};   /* wifi_ssid 最大 63 字节 */
    char pass_esc[64 * 6 + 1]  = {0};   /* wifi_password 最大 63 字节 */
    char url_esc[128 * 6 + 1]  = {0};   /* server_url 最大 127 字节 */
    html_escape(ssid_esc, sizeof(ssid_esc),   cfg->wifi_ssid);
    html_escape(pass_esc, sizeof(pass_esc),   cfg->wifi_password);
    html_escape(url_esc,  sizeof(url_esc),    cfg->server_url);

    /* 当前分辨率下拉项 selected 标记 */
    const char *sel_uxga = (strcmp(cfg->resolution, "1600x1200") == 0) ? "selected" : "";
    const char *sel_sxga = (strcmp(cfg->resolution, "1280x1024") == 0) ? "selected" : "";
    const char *sel_vga  = (strcmp(cfg->resolution, "640x480")   == 0) ? "selected" : "";
    const char *upload_checked = cfg->upload_enabled ? "checked" : "";

    char *html = (char *)malloc(HTML_PAGE_MAX_LEN);
    if (html == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    /* 拼装 mobile-friendly HTML 表单 */
    int len = snprintf(html, HTML_PAGE_MAX_LEN,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'>"
        "<title>户外相机配置</title>"
        "<style>"
        "*{box-sizing:border-box;}"
        "body{font-family:-apple-system,sans-serif;margin:0;padding:16px;"
        "background:#f0f2f5;color:#333;}"
        ".card{max-width:480px;margin:0 auto;background:#fff;padding:20px;"
        "border-radius:10px;box-shadow:0 2px 8px rgba(0,0,0,0.08);}"
        "h2{margin:0 0 16px;color:#1a73e8;font-size:20px;}"
        "label{display:block;margin:14px 0 6px;font-size:14px;color:#555;font-weight:600;}"
        "input[type=text],input[type=password],input[type=number],select{"
        "width:100%%;padding:10px;border:1px solid #ddd;border-radius:6px;"
        "font-size:15px;}"
        ".switch-row{display:flex;align-items:center;margin:14px 0;}"
        ".switch-row input{width:20px;height:20px;margin-right:10px;}"
        ".switch-row label{margin:0;font-size:15px;}"
        "button{margin-top:24px;width:100%%;padding:14px;background:#1a73e8;"
        "color:#fff;border:none;border-radius:6px;font-size:16px;font-weight:600;cursor:pointer;}"
        "button:active{background:#1557b0;}"
        ".hint{font-size:12px;color:#999;margin-top:4px;}"
        "</style></head><body><div class='card'>"
        "<h2>📷 户外相机配置</h2>"
        "<form action='/save' method='post'>"
        "<label>拍照间隔（分钟）</label>"
        "<input type='number' name='interval_minutes' min='1' max='1440' value='%u'>"
        "<label>分辨率</label>"
        "<select name='resolution'>"
        "<option value='1600x1200' %s>UXGA (1600x1200)</option>"
        "<option value='1280x1024' %s>SXGA (1280x1024)</option>"
        "<option value='640x480' %s>VGA (640x480)</option>"
        "</select>"
        "<label>JPEG 质量（1-63，数值越小质量越高）</label>"
        "<input type='number' name='quality' min='1' max='63' value='%d'>"
        "<div class='switch-row'>"
        "<input type='checkbox' name='upload_enabled' %s>"
        "<label for='upload_enabled'>启用 WiFi 回传</label>"
        "</div>"
        "<label>WiFi SSID</label>"
        "<input type='text' name='wifi_ssid' maxlength='63' value='%s' placeholder='WiFi 名称'>"
        "<label>WiFi 密码</label>"
        "<input type='password' name='wifi_password' maxlength='63' value='%s' placeholder='WiFi 密码'>"
        "<label>服务器 URL</label>"
        "<input type='text' name='server_url' maxlength='127' value='%s' placeholder='http://example.com/upload'>"
        "<div class='hint'>照片将以 image/jpeg 形式 POST 到该 URL</div>"
        "<button type='submit'>💾 保存并重启</button>"
        "</form></div></body></html>",
        (unsigned)cfg->interval_minutes,
        sel_uxga, sel_sxga, sel_vga,
        cfg->quality,
        upload_checked,
        ssid_esc, pass_esc, url_esc);

    if (len < 0 || len >= HTML_PAGE_MAX_LEN) {
        ESP_LOGE(TAG, "HTML 拼装失败/被截断 (len=%d)", len);
        free(html);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, len);
    free(html);
    return ESP_OK;
}

/**
 * @brief POST /save - 解析表单，更新 config_t 并保存到 SD 卡，3 秒后重启
 */
static esp_err_t portal_post_save(httpd_req_t *req)
{
    /* 1. 接收 POST body（限制最大长度） */
    int total_len = (int)req->content_len;
    if (total_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (total_len >= POST_BODY_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    char buf[POST_BODY_MAX_LEN] = {0};
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret < 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  /* 超时则继续等待 */
            }
            ESP_LOGE(TAG, "接收 body 失败: ret=%d", ret);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (ret == 0) {
            /* 连接关闭 */
            break;
        }
        received += ret;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "收到 POST /save: %s", buf);

    /* 2. 以当前配置为基准创建可变副本，更新字段后用 config_mgr_set 写回 */
    const config_t *cur = config_mgr_get();
    config_t new_cfg;
    if (cur != NULL) {
        new_cfg = *cur;             /* 拷贝当前配置 */
    } else {
        new_cfg = *config_mgr_get_default();  /* 未加载则从默认值开始 */
    }

    char field[FORM_FIELD_MAX_LEN] = {0};

    if (form_get_value(buf, "interval_minutes", field, sizeof(field))) {
        int v = atoi(field);
        if (v >= 1 && v <= 1440) {
            new_cfg.interval_minutes = (uint16_t)v;
        }
    }
    if (form_get_value(buf, "resolution", field, sizeof(field))) {
        /* 仅允许三种合法分辨率 */
        if (strcmp(field, "1600x1200") == 0 ||
            strcmp(field, "1280x1024") == 0 ||
            strcmp(field, "640x480") == 0) {
            strncpy(new_cfg.resolution, field, sizeof(new_cfg.resolution) - 1);
            new_cfg.resolution[sizeof(new_cfg.resolution) - 1] = '\0';
        }
    }
    if (form_get_value(buf, "quality", field, sizeof(field))) {
        int v = atoi(field);
        if (v >= 1 && v <= 63) {
            new_cfg.quality = v;
        }
    }
    /* checkbox：表单中存在该字段即表示勾选 */
    new_cfg.upload_enabled = form_get_value(buf, "upload_enabled",
                                            field, sizeof(field));

    if (form_get_value(buf, "wifi_ssid", field, sizeof(field))) {
        strncpy(new_cfg.wifi_ssid, field, sizeof(new_cfg.wifi_ssid) - 1);
        new_cfg.wifi_ssid[sizeof(new_cfg.wifi_ssid) - 1] = '\0';
    }
    if (form_get_value(buf, "wifi_password", field, sizeof(field))) {
        strncpy(new_cfg.wifi_password, field, sizeof(new_cfg.wifi_password) - 1);
        new_cfg.wifi_password[sizeof(new_cfg.wifi_password) - 1] = '\0';
    }
    if (form_get_value(buf, "server_url", field, sizeof(field))) {
        strncpy(new_cfg.server_url, field, sizeof(new_cfg.server_url) - 1);
        new_cfg.server_url[sizeof(new_cfg.server_url) - 1] = '\0';
    }

    /* 3. 写回 config_mgr 并持久化到 SD 卡 */
    esp_err_t set_ret = config_mgr_set(&new_cfg);
    if (set_ret != ESP_OK) {
        ESP_LOGE(TAG, "config_mgr_set 失败: %s", esp_err_to_name(set_ret));
    }

    esp_err_t save_ret = config_mgr_save();
    if (save_ret != ESP_OK) {
        ESP_LOGE(TAG, "配置保存失败: %s", esp_err_to_name(save_ret));
        logger_log(LOG_LEVEL_ERROR, "config_save_failed",
                   "portal: %s", esp_err_to_name(save_ret));
    } else {
        ESP_LOGI(TAG, "配置已保存到 SD 卡");
        logger_log(LOG_LEVEL_INFO, "config_saved", "via portal");
    }

    /* 4. 标记已收到，让主循环退出 */
    s_save_received = true;

    /* 5. 返回成功页面 */
    static const char *resp_html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>保存成功</title>"
        "<style>"
        "body{font-family:-apple-system,sans-serif;text-align:center;padding:60px 20px;background:#f0f2f5;}"
        ".card{max-width:400px;margin:0 auto;background:#fff;padding:40px 20px;border-radius:10px;box-shadow:0 2px 8px rgba(0,0,0,0.08);}"
        "h2{color:#34a853;margin:0 0 12px;}"
        "p{color:#666;margin:8px 0;}"
        ".spinner{margin:20px auto;width:36px;height:36px;border:4px solid #e0e0e0;border-top-color:#1a73e8;border-radius:50%%;animation:spin 1s linear infinite;}"
        "@keyframes spin{to{transform:rotate(360deg);}}"
        "</style></head><body><div class='card'>"
        "<h2>✅ 保存成功</h2>"
        "<p>设备将在 3 秒后自动重启</p>"
        "<div class='spinner'></div>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, resp_html, HTTPD_RESP_USE_STRLEN);

    /* 6. 延迟 3 秒后重启（确保响应已发出） */
    vTaskDelay(pdMS_TO_TICKS(PORTAL_RESTART_DELAY_MS));
    ESP_LOGI(TAG, "配置已保存，重启设备...");
    esp_restart();

    /* 不会执行到此处 */
    return ESP_OK;
}

/**
 * @brief GET /status - 返回当前状态 JSON
 *
 * 包含电池电压/百分比、SD 卡剩余空间与当前配置。
 */
static esp_err_t portal_get_status(httpd_req_t *req)
{
    /* config_mgr_get 在未加载时返回 NULL，回退到默认配置 */
    const config_t *cfg = config_mgr_get();
    if (cfg == NULL) {
        cfg = config_mgr_get_default();
    }

    /* 采样电池电压（失败则填 0） */
    float vbat = 0.0f;
    power_read_battery_voltage(&vbat);
    float pct = power_get_battery_percent(vbat);

    /* 查询 SD 卡空间（失败则填 0） */
    uint64_t free_bytes = 0, total_bytes = 0;
    sd_storage_get_free_space(&free_bytes, &total_bytes);
    uint64_t free_mb = free_bytes / (1024 * 1024);

    /* 拼装 JSON 状态（简化：字段值未做 JSON 转义，配置值由本设备写入） */
    char json[768] = {0};
    int len = snprintf(json, sizeof(json),
        "{\"battery_v\":%.2f,\"battery_pct\":%.0f,\"sd_free_mb\":%llu,"
        "\"config\":{\"interval_minutes\":%u,\"resolution\":\"%s\","
        "\"quality\":%d,\"upload_enabled\":%s,"
        "\"wifi_ssid\":\"%s\",\"server_url\":\"%s\"}}",
        (double)vbat, (double)pct, (unsigned long long)free_mb,
        (unsigned)cfg->interval_minutes, cfg->resolution,
        cfg->quality, cfg->upload_enabled ? "true" : "false",
        cfg->wifi_ssid, cfg->server_url);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len > 0 ? len : 0);
    return ESP_OK;
}

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t config_portal_start(void)
{
    /* 1. NVS 初始化（WiFi 需要；已初始化则忽略） */
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

    /* 2. 网络接口与默认事件循环（已创建则忽略错误） */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "事件循环创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 创建默认 AP netif */
    s_ap_netif = esp_netif_create_default_wifi_ap();

    /* 4. 初始化 WiFi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_wifi_inited = true;

    /* 5. 配置 SoftAP（SSID = OutdoorCam-XXXX，密码固定） */
    char ap_ssid[32] = {0};
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));

    wifi_config_t ap_config = {
        .ap = {
            .channel         = PORTAL_AP_CHANNEL,
            .max_connection  = PORTAL_AP_MAX_CONN,
            .authmode        = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg         = { .required = false },
            .ssid_hidden     = 0,
            .beacon_interval = 100,
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, PORTAL_AP_PASSWORD,
            sizeof(ap_config.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP 已启动: SSID=%s, password=%s, channel=%d",
             ap_ssid, PORTAL_AP_PASSWORD, PORTAL_AP_CHANNEL);

    /* 6. 启动 HTTP 服务器 */
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers   = 4;
    http_config.lru_purge_enable   = true;
    http_config.stack_size         = 8192;
    http_config.max_resp_headers   = 8;

    ret = httpd_start(&s_server, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server 启动失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 7. 注册 URI 处理器 */
    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = portal_get_root
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = portal_post_save
    };
    static const httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET, .handler = portal_get_status
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_status);

    ESP_LOGI(TAG, "配置门户已启动，访问 http://192.168.4.1/ 进行配置");
    logger_log(LOG_LEVEL_INFO, "portal_started", "AP=%s", ap_ssid);

    /* 8. 阻塞运行，直到收到 /save 或超时（30 分钟）
     *    注：实际可改为持续运行直到设备重启 */
    s_save_received = false;
    uint32_t elapsed = 0;
    while (!s_save_received && elapsed < PORTAL_RUN_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(PORTAL_POLL_STEP_MS));
        elapsed += PORTAL_POLL_STEP_MS;
    }

    if (!s_save_received) {
        ESP_LOGW(TAG, "配置门户运行超时（%d 分钟），自动关闭",
                 PORTAL_RUN_TIMEOUT_MS / 60000);
        logger_log(LOG_LEVEL_WARN, "portal_timeout", "%d min",
                   PORTAL_RUN_TIMEOUT_MS / 60000);
        config_portal_stop();
    }
    /* 若 s_save_received 为真，portal_post_save 已调用 esp_restart()，不会执行到此处 */
    return ESP_OK;
}

esp_err_t config_portal_stop(void)
{
    /* 1. 停止 HTTP 服务器 */
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    /* 2. 停止并释放 WiFi */
    if (s_wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_inited = false;
    }

    /* 3. 释放 AP netif */
    if (s_ap_netif != NULL) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }

    ESP_LOGI(TAG, "配置门户已停止");
    return ESP_OK;
}
