#include "app_main.h"

#include "app.h"
#include "config_mgr.h"
#include "config_portal.h"
#include "logger.h"
#include "power_mgmt.h"
#include "sd_storage.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* 配置按钮长按检测阈值（毫秒） */
#define CONFIG_BTN_LONG_PRESS_MS    5000
/* 配置按钮轮询间隔（毫秒） */
#define CONFIG_BTN_POLL_INTERVAL_MS 100

/**
 * @brief 检测 GPIO0 配置按钮长按 5 秒
 *
 * 上电后若 GPIO0 持续低电平 5 秒，则启动配置门户。
 * 简化实现：阻塞轮询；实际产品可改为中断+定时器。
 *
 * @return true 长按确认；false 未触发
 */
static bool check_config_button_long_press(void)
{
    /* GPIO0 已在 power_mgmt_init 中配置为输入上拉 */
    if (gpio_get_level(GPIO_NUM_0) != 0) {
        return false;   /* 未按下，正常流程 */
    }

    ESP_LOGI(TAG, "检测到 GPIO0 按下，等待 %d ms 确认长按",
             CONFIG_BTN_LONG_PRESS_MS);

    int total_checks = CONFIG_BTN_LONG_PRESS_MS / CONFIG_BTN_POLL_INTERVAL_MS;
    for (int i = 0; i < total_checks; i++) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BTN_POLL_INTERVAL_MS));
        if (gpio_get_level(GPIO_NUM_0) != 0) {
            ESP_LOGI(TAG, "GPIO0 已释放（第 %d 次检测），取消长按", i + 1);
            return false;
        }
    }
    return true;
}

/**
 * @brief 程序入口
 *
 * 完整启动流程（与规格说明一致）：
 *   1. 打印固件/芯片信息
 *   2. 检查唤醒原因（EXT0=RTC 闹钟 / POWERON=首次上电）
 *   3. 电源管理初始化（ADC 电池采样、外设电源控制 GPIO）
 *   4. 短暂上电 SD 卡，初始化日志系统并加载配置
 *   5. 检测配置按钮长按 5 秒 → 启动配置门户
 *   6. 进入 app_run() 主业务循环（拍照→存储→回传→睡眠）
 *
 * 注意：app_run() 内部最终会调用 power_enter_deep_sleep()，由 RTC 闹钟
 *       唤醒后系统复位，重新执行 app_main()，形成"醒-拍-睡"循环。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, " 户外定时拍照固件启动 (ESP32-S3)");
    ESP_LOGI(TAG, " ESP-IDF version : %s", esp_get_idf_version());
    ESP_LOGI(TAG, " Chip revision   : %d", esp_chip_get_revision());
    ESP_LOGI(TAG, " Free heap       : %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "============================================");

    /* 1. 检查唤醒原因 */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
        ESP_LOGI(TAG, "唤醒原因: EXT0 (DS3231 闹钟唤醒)");
        break;
    case ESP_SLEEP_WAKEUP_EXT1:
        ESP_LOGI(TAG, "唤醒原因: EXT1 (GPIO 唤醒，可能是配置按钮)");
        break;
    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG, "唤醒原因: TIMER (兜底定时器唤醒)");
        break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
        ESP_LOGI(TAG, "唤醒原因: POWERON / 复位 (首次上电或看门狗)");
        break;
    }

    /* 2. 电源管理初始化（配置 GPIO/ADC） */
    power_mgmt_init();

    /* 3. 短暂上电 SD 卡，读取配置 + 初始化日志 */
    power_enable_peripheral(P_SD);
    esp_err_t sd_ret = sd_storage_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡初始化失败: %s，日志/配置将不可用",
                 esp_err_to_name(sd_ret));
        /* SD 失败仍继续：logger_init 会失败，config_mgr_load 用默认值 */
    }

    /* 日志系统初始化（依赖 SD 卡挂载） */
    esp_err_t log_ret = logger_init("/sdcard/logs");
    if (log_ret != ESP_OK) {
        ESP_LOGW(TAG, "日志系统初始化失败: %s（仅 ESP_LOG 输出）",
                 esp_err_to_name(log_ret));
    }

    /* 配置加载（SD 失败时使用默认值，不阻塞启动） */
    esp_err_t cfg_ret = config_mgr_load();
    if (cfg_ret != ESP_OK) {
        ESP_LOGW(TAG, "配置加载失败，使用内置默认配置");
    }

    /* 4. 检测配置按钮长按 5 秒 → 启动配置门户 */
    if (check_config_button_long_press()) {
        ESP_LOGI(TAG, "启动配置门户（SoftAP + HTTP Server）");
        (void)logger_log(LOG_LEVEL_INFO, "config_portal", "started (GPIO0 long press)");
        config_portal_start();
        /* 配置门户退出后重新加载配置 */
        config_mgr_load();
        ESP_LOGI(TAG, "配置门户已退出，继续正常流程");
    }

    /* 5. 进入主业务循环（正常情况下不返回，最终进入深度睡眠） */
    app_run();

    /* 6. 兜底：app_run 异常返回（不应发生），1 分钟后软复位 */
    ESP_LOGE(TAG, "app_run() 异常返回，1 分钟后重启");
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    esp_restart();
}
