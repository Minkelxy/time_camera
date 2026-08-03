#include "app.h"

#include "camera_hal.h"
#include "config_mgr.h"
#include "ds3231.h"
#include "logger.h"
#include "power_mgmt.h"
#include "sd_storage.h"
#include "wifi_upload.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "app";

/* I2C 引脚配置（与 gpio_allocation.md 一致：DS3231 与 OV2640 SCCB 共享 I2C0） */
#define APP_I2C_PORT        I2C_NUM_0
#define APP_I2C_SDA         GPIO_NUM_4
#define APP_I2C_SCL         GPIO_NUM_5

/* DS3231 闹钟中断引脚（深度睡眠 EXT0 唤醒源） */
#define APP_RTC_INT_GPIO    GPIO_NUM_21

/* 各关键步骤重试次数 */
#define SD_INIT_RETRY_COUNT     2
#define CAM_INIT_RETRY_COUNT    3
#define CAPTURE_RETRY_COUNT     3
#define WRITE_RETRY_COUNT       3
#define UPLOAD_RETRY_COUNT      2

/* SD 卡可用空间低于此百分比时清理最早日期目录 */
#define SD_FREE_PERCENT_THRESHOLD   10

/* ============================================================ */
/*                          内部辅助函数                         */
/* ============================================================ */

/**
 * @brief logger 时间源回调：包装 ds3231_get_time
 *
 * 通过 logger_set_time_source 注入，使 logger 输出的时间戳来自 RTC，
 * 避免 logger 组件直接依赖 ds3231 造成循环依赖。
 */
static void app_logger_time_source(struct tm *out)
{
    if (out == NULL) {
        return;
    }
    if (ds3231_get_time(out) != ESP_OK) {
        /* RTC 读取失败时清零，避免输出脏数据 */
        memset(out, 0, sizeof(*out));
    }
}

/**
 * @brief 构造照片保存路径 /sdcard/photos/YYYYMMDD/HHMMSS.jpg
 *
 * 同时输出日期目录路径，供调用方显式 mkdir。
 *
 * @param path_out     路径输出缓冲（建议 >= 48 字节）
 * @param path_len     路径缓冲长度
 * @param date_dir_out 日期目录输出缓冲（可为 NULL，建议 >= 32 字节）
 * @param date_dir_len 日期目录缓冲长度
 * @return ESP_OK 成功；ESP_FAIL RTC 时间读取失败
 */
static esp_err_t build_photo_path(char *path_out, size_t path_len,
                                  char *date_dir_out, size_t date_dir_len)
{
    struct tm tm_now = {0};
    esp_err_t ret = ds3231_get_time(&tm_now);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC 时间读取失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (date_dir_out != NULL && date_dir_len > 0) {
        strftime(date_dir_out, date_dir_len, "/sdcard/photos/%Y%m%d", &tm_now);
    }
    if (path_out != NULL && path_len > 0) {
        strftime(path_out, path_len, "/sdcard/photos/%Y%m%d/%H%M%S.jpg", &tm_now);
    }
    return ESP_OK;
}

/**
 * @brief 检查 SD 卡剩余空间，不足阈值时清理最早日期目录
 *
 * /sdcard/photos 下按日期建子目录（YYYYMMDD），list_dir 返回已排序的
 * 子目录列表，首项即最早日期，删除以释放空间。
 */
static void cleanup_sd_if_needed(void)
{
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    if (sd_storage_get_free_space(&free_bytes, &total_bytes) != ESP_OK) {
        return;
    }
    if (total_bytes == 0) {
        return;
    }
    uint32_t free_percent = (uint32_t)((free_bytes * 100) / total_bytes);
    if (free_percent >= SD_FREE_PERCENT_THRESHOLD) {
        return;
    }

    ESP_LOGW(TAG, "SD 卡可用空间 %lu%%，清理最早日期目录", (unsigned long)free_percent);
    logger_log(LOG_LEVEL_WARN, "cleanup", "SD free %lu%%, cleaning oldest",
               (unsigned long)free_percent);

    char **list = NULL;
    size_t count = 0;
    if (sd_storage_list_dir("/sdcard/photos", &list, &count) != ESP_OK) {
        return;
    }
    /* 仅当存在多个日期目录时才删除最早一个，避免清空当天所有照片 */
    if (count > 1 && list != NULL && list[0] != NULL) {
        char dir_path[64] = {0};
        snprintf(dir_path, sizeof(dir_path), "/sdcard/photos/%s", list[0]);
        if (sd_storage_delete_path(dir_path) == ESP_OK) {
            logger_log(LOG_LEVEL_INFO, "cleanup", "deleted %s", dir_path);
        } else {
            logger_log(LOG_LEVEL_ERROR, "cleanup", "delete %s failed", dir_path);
        }
    }
    /* 释放 list_dir 返回的字符串数组（调用方负责 free） */
    for (size_t i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

/**
 * @brief 关闭所有外设并反初始化（错误兜底用）
 *
 * 保证任何失败分支退出前都释放资源，避免泄漏导致后续周期失败。
 */
static void shutdown_peripherals(void)
{
    camera_hal_deinit();
    sd_storage_deinit();
    power_disable_peripheral(P_CAM);
    power_disable_peripheral(P_SD);
}

/**
 * @brief 错误兜底：清理资源、设 RTC 闹钟、进入深度睡眠
 *
 * 各关键步骤重试耗尽后调用，保证系统不会卡死，下次 RTC 唤醒重试。
 *
 * @param interval 下次唤醒间隔（分钟）
 */
static void abort_cycle_and_sleep(uint8_t interval)
{
    shutdown_peripherals();
    ds3231_clear_alarm();
    ds3231_set_alarm(interval);
    ESP_LOGI(TAG, "进入深度睡眠（错误兜底），下次唤醒间隔 %u 分钟", interval);
    power_enter_deep_sleep(0, APP_RTC_INT_GPIO);
}

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t app_run(void)
{
    ESP_LOGI(TAG, "==== 业务周期开始 ====");

    /* ---- 1. 初始化 RTC，注册 logger 时间源 ---- */
    esp_err_t ret = ds3231_init(APP_I2C_PORT, APP_I2C_SDA, APP_I2C_SCL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 初始化失败: %s", esp_err_to_name(ret));
        /* RTC 失败无法设闹钟，只能靠 timer 兜底唤醒重试 */
        (void)logger_log(LOG_LEVEL_ERROR, "rtc", "init failed: %s", esp_err_to_name(ret));
        power_enter_deep_sleep(300, 0);   /* 5 分钟 timer 兜底 */
        return ret;
    }
    logger_set_time_source(app_logger_time_source);

    /* 读取 RTC 时间并记录 */
    struct tm tm_now = {0};
    ds3231_get_time(&tm_now);
    ESP_LOGI(TAG, "RTC 时间: %04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    /* ---- 2. 读取电池电压 ---- */
    float vbat = 0.0f;
    power_read_battery_voltage(&vbat);
    float batt_percent = power_get_battery_percent(vbat);
    ESP_LOGI(TAG, "电池电压: %.2fV (%.0f%%)", vbat, batt_percent);

    /* ---- 4. 读取配置（load 失败时回退默认） ---- */
    const config_t *cfg = config_mgr_get();
    if (cfg == NULL) {
        cfg = config_mgr_get_default();
        ESP_LOGW(TAG, "配置未加载，使用内置默认值");
    }

    /* ---- 3. 低电量保护：决定是否跳过拍照 / 调整间隔 ----
     * 注意：先读配置再判断，因为 power_should_skip_capture 需要传入 interval。
     * config_t.interval_minutes 为 uint16_t，需 clamp 到 uint8_t 供 API 使用 */
    uint8_t adjusted_interval = (cfg->interval_minutes > 255) ?
                                255 : (uint8_t)cfg->interval_minutes;
    bool skip = power_should_skip_capture(vbat, &adjusted_interval);
    if (skip) {
        ESP_LOGW(TAG, "电量过低 (%.2fV)，跳过本次拍照，直接进入睡眠", vbat);
        (void)logger_log(LOG_LEVEL_WARN, "battery", "skip capture: %.2fV", vbat);
        (void)logger_log(LOG_LEVEL_INFO, "sleep", "next wake in %u min", adjusted_interval);

        /* 释放 main.c 中已申请的 SD / logger 资源，保证数据落盘 */
        logger_deinit();
        sd_storage_deinit();
        power_disable_peripheral(P_SD);

        ds3231_clear_alarm();
        ds3231_set_alarm(adjusted_interval);
        ESP_LOGI(TAG, "进入深度睡眠（低电量跳过），下次唤醒间隔 %u 分钟", adjusted_interval);
        power_enter_deep_sleep(0, APP_RTC_INT_GPIO);
        return ESP_OK;   /* 不会执行到 */
    }

    (void)logger_log(LOG_LEVEL_INFO, "boot",
               "RTC %04d-%02d-%02d %02d:%02d:%02d, Vbat %.2fV (%.0f%%), interval %u min, res %s, upload %d",
               tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
               tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
               vbat, batt_percent, adjusted_interval,
               cfg->resolution, (int)cfg->upload_enabled);

    /* ---- 5. 上电外设 ---- */
    power_enable_peripheral(P_SD);
    power_enable_peripheral(P_CAM);

    /* ---- 6. 初始化 SD 卡（失败重试 2 次） ----
     * SD 卡可能在 main.c 中已挂载，sd_storage_init 内部幂等处理 */
    esp_err_t sd_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_INIT_RETRY_COUNT; attempt++) {
        sd_ret = sd_storage_init();
        if (sd_ret == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "SD 卡初始化失败 (attempt %d/%d): %s",
                 attempt, SD_INIT_RETRY_COUNT, esp_err_to_name(sd_ret));
        (void)logger_log(LOG_LEVEL_ERROR, "sd", "init failed attempt %d: %s",
                         attempt, esp_err_to_name(sd_ret));
        if (attempt < SD_INIT_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡初始化重试 %d 次仍失败，放弃本次周期", SD_INIT_RETRY_COUNT);
        (void)logger_log(LOG_LEVEL_ERROR, "sd", "init failed after %d retries", SD_INIT_RETRY_COUNT);
        logger_deinit();
        abort_cycle_and_sleep(adjusted_interval);
        return ESP_OK;
    }

    /* ---- 7. 初始化摄像头（失败重试 3 次，每次重新上下电） ---- */
    esp_err_t cam_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= CAM_INIT_RETRY_COUNT; attempt++) {
        cam_ret = camera_hal_init();
        if (cam_ret == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "摄像头初始化失败 (attempt %d/%d): %s",
                 attempt, CAM_INIT_RETRY_COUNT, esp_err_to_name(cam_ret));
        (void)logger_log(LOG_LEVEL_ERROR, "camera", "init failed attempt %d: %s",
                         attempt, esp_err_to_name(cam_ret));
        if (attempt < CAM_INIT_RETRY_COUNT) {
            /* 重新上下电摄像头再试 */
            power_disable_peripheral(P_CAM);
            vTaskDelay(pdMS_TO_TICKS(200));
            power_enable_peripheral(P_CAM);
        }
    }
    if (cam_ret != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化重试 %d 次仍失败", CAM_INIT_RETRY_COUNT);
        (void)logger_log(LOG_LEVEL_ERROR, "camera", "init failed after %d retries", CAM_INIT_RETRY_COUNT);
        logger_deinit();
        abort_cycle_and_sleep(adjusted_interval);
        return ESP_OK;
    }

    /* ---- 8. 拍照（失败重试 3 次，每次 re-init 摄像头） ---- */
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    bool captured = false;
    for (int attempt = 1; attempt <= CAPTURE_RETRY_COUNT; attempt++) {
        ret = camera_hal_capture(&frame, &frame_len,
                                 cfg->resolution, cfg->quality);
        if (ret == ESP_OK && frame != NULL && frame_len > 0) {
            captured = true;
            break;
        }
        ESP_LOGE(TAG, "拍照失败 (attempt %d/%d): %s",
                 attempt, CAPTURE_RETRY_COUNT, esp_err_to_name(ret));
        (void)logger_log(LOG_LEVEL_ERROR, "capture", "failed attempt %d: %s",
                         attempt, esp_err_to_name(ret));
        /* 归还可能的帧缓冲，防止泄漏 */
        camera_hal_release_fb();
        if (attempt < CAPTURE_RETRY_COUNT) {
            /* re-init 摄像头再试 */
            camera_hal_deinit();
            power_disable_peripheral(P_CAM);
            vTaskDelay(pdMS_TO_TICKS(200));
            power_enable_peripheral(P_CAM);
            if (camera_hal_init() != ESP_OK) {
                ESP_LOGE(TAG, "摄像头 re-init 失败，继续重试");
            }
        }
    }
    if (!captured) {
        ESP_LOGE(TAG, "拍照重试 %d 次仍失败", CAPTURE_RETRY_COUNT);
        (void)logger_log(LOG_LEVEL_ERROR, "capture", "failed after %d retries", CAPTURE_RETRY_COUNT);
        logger_deinit();
        abort_cycle_and_sleep(adjusted_interval);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "拍照成功: %u 字节", (unsigned)frame_len);

    /* ---- 9. 构造保存路径并创建日期目录 ---- */
    char photo_path[48] = {0};
    char date_dir[32] = {0};
    if (build_photo_path(photo_path, sizeof(photo_path),
                         date_dir, sizeof(date_dir)) != ESP_OK) {
        /* RTC 时间读取失败的兜底：用系统 tick 作为唯一标识 */
        snprintf(photo_path, sizeof(photo_path),
                 "/sdcard/photos/unknown_%llu.jpg",
                 (unsigned long long)(esp_timer_get_time() / 1000000));
        snprintf(date_dir, sizeof(date_dir), "/sdcard/photos");
    }
    sd_storage_mkdir(date_dir);

    /* ---- 10. 写入文件（失败重试 3 次） ---- */
    esp_err_t write_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= WRITE_RETRY_COUNT; attempt++) {
        write_ret = sd_storage_write_file(photo_path, frame, frame_len);
        if (write_ret == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "照片写入失败 (attempt %d/%d): %s",
                 attempt, WRITE_RETRY_COUNT, esp_err_to_name(write_ret));
        (void)logger_log(LOG_LEVEL_ERROR, "storage", "write failed attempt %d: %s",
                         attempt, esp_err_to_name(write_ret));
        if (attempt < WRITE_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    /* ---- 11. 释放帧缓冲（无论写入是否成功都要归还） ---- */
    camera_hal_release_fb();

    /* ---- 12. 关闭摄像头 ---- */
    camera_hal_deinit();

    if (write_ret != ESP_OK) {
        ESP_LOGE(TAG, "照片写入重试 %d 次仍失败: %s",
                 WRITE_RETRY_COUNT, photo_path);
        (void)logger_log(LOG_LEVEL_ERROR, "storage", "write failed after %d retries: %s",
                         WRITE_RETRY_COUNT, photo_path);
    } else {
        ESP_LOGI(TAG, "照片已保存: %s (%u 字节)", photo_path, (unsigned)frame_len);
        (void)logger_log(LOG_LEVEL_INFO, "photo_saved", "%s (%u bytes)",
                         photo_path, (unsigned)frame_len);
    }

    /* ---- 13. SD 卡空间检查与清理 ---- */
    cleanup_sd_if_needed();

    /* ---- 14. WiFi 回传（失败不影响主流程，照片已存本地） ---- */
    if (cfg->upload_enabled && cfg->wifi_ssid[0] != '\0' && cfg->server_url[0] != '\0') {
        bool uploaded = false;
        for (int attempt = 1; attempt <= UPLOAD_RETRY_COUNT; attempt++) {
            esp_err_t init_ret = wifi_upload_init(cfg->wifi_ssid, cfg->wifi_password);
            if (init_ret != ESP_OK) {
                ESP_LOGW(TAG, "WiFi 模块初始化失败 (attempt %d): %s",
                         attempt, esp_err_to_name(init_ret));
                (void)logger_log(LOG_LEVEL_WARN, "upload", "init failed attempt %d: %s",
                                 attempt, esp_err_to_name(init_ret));
                if (attempt < UPLOAD_RETRY_COUNT) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                continue;
            }

            esp_err_t conn_ret = wifi_upload_connect();
            if (conn_ret == ESP_OK) {
                esp_err_t send_ret = wifi_upload_send_photo(photo_path, cfg->server_url);
                wifi_upload_disconnect();
                if (send_ret == ESP_OK) {
                    uploaded = true;
                    ESP_LOGI(TAG, "照片已上传 (attempt %d)", attempt);
                    (void)logger_log(LOG_LEVEL_INFO, "upload", "ok: %s", photo_path);
                    wifi_upload_deinit();
                    break;
                }
                ESP_LOGW(TAG, "上传失败 (attempt %d): %s", attempt, esp_err_to_name(send_ret));
                (void)logger_log(LOG_LEVEL_WARN, "upload", "send failed attempt %d: %s",
                                 attempt, esp_err_to_name(send_ret));
            } else {
                ESP_LOGW(TAG, "WiFi 连接失败 (attempt %d): %s", attempt, esp_err_to_name(conn_ret));
                (void)logger_log(LOG_LEVEL_WARN, "upload", "connect failed attempt %d: %s",
                                 attempt, esp_err_to_name(conn_ret));
            }
            wifi_upload_deinit();
            if (attempt < UPLOAD_RETRY_COUNT) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        if (!uploaded) {
            ESP_LOGW(TAG, "上传重试 %d 次仍失败（照片已存本地）", UPLOAD_RETRY_COUNT);
            (void)logger_log(LOG_LEVEL_ERROR, "upload",
                             "failed after %d retries (photo saved locally)", UPLOAD_RETRY_COUNT);
        }
    } else if (cfg->upload_enabled) {
        ESP_LOGW(TAG, "upload_enabled=true 但 ssid 或 server_url 为空，跳过上传");
        (void)logger_log(LOG_LEVEL_WARN, "upload", "skipped: ssid or url empty");
    }

    /* ---- 15. 关闭 SD 卡 ---- */
    sd_storage_deinit();

    /* ---- 16. 关闭外设电源 ---- */
    power_disable_peripheral(P_SD);
    power_disable_peripheral(P_CAM);

    /* ---- 17. 设置 RTC 闹钟 ---- */
    ds3231_clear_alarm();
    ds3231_set_alarm(adjusted_interval);

    /* ---- 18. 记录关键日志 ---- */
    (void)logger_log(LOG_LEVEL_INFO, "cycle_done",
                     "photo=%s size=%u interval=%u vbat=%.2f",
                     photo_path, (unsigned)frame_len, adjusted_interval, vbat);

    /* 关闭日志系统（flush 并关闭文件），保证睡眠前数据落盘 */
    logger_deinit();

    /* ---- 19. 进入深度睡眠 ----
     * seconds=0：不启用 timer 兜底，完全依赖 DS3231 闹钟唤醒。
     * wake_gpio=GPIO21：与 power_mgmt 内部 EXT0 配置的 PIN_RTC_INT 一致，
     *   power_mgmt 会跳过重复的 EXT1 配置，仅靠 EXT0 唤醒。
     * 唤醒后系统复位，重新执行 app_main()，形成"醒-拍-睡"循环。 */
    ESP_LOGI(TAG, "==== 进入深度睡眠，下次唤醒间隔 %u 分钟 ====", adjusted_interval);
    power_enter_deep_sleep(0, APP_RTC_INT_GPIO);

    /* 正常情况下不会执行到此处（深度睡眠即系统复位） */
    return ESP_OK;
}
