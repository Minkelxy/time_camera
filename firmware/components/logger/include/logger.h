#pragma once

#include <stdarg.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 日志级别枚举
 */
typedef enum {
    LOG_LEVEL_INFO = 0,   ///< 信息
    LOG_LEVEL_WARN,       ///< 警告
    LOG_LEVEL_ERROR,      ///< 错误
} log_level_t;

/**
 * @brief 初始化日志系统
 *
 * 在 SD 卡指定目录下创建日志文件，同时输出到 ESP_LOG（串口）。
 * 支持按文件大小轮转。
 *
 * @param log_dir 日志目录（如 "/sdcard/logs"）
 * @return ESP_OK 成功
 */
esp_err_t logger_init(const char *log_dir);

/**
 * @brief 写入一条日志
 *
 * 同时输出到 ESP_LOG 与 SD 卡文件。
 *
 * @param level 日志级别
 * @param event 事件标签（如 "photo_saved" / "battery"）
 * @param fmt   printf 风格格式串
 * @param ...   可变参数
 * @return ESP_OK 成功
 */
esp_err_t logger_log(log_level_t level, const char *event, const char *fmt, ...);

/**
 * @brief 设置时间源回调函数
 *
 * 用于注入 RTC（如 DS3231）时间获取函数，避免 logger 组件直接依赖 ds3231
 * 造成循环依赖。若未注入，则使用系统时间 localtime(time(NULL))。
 *
 * @param get_time 时间获取回调，传入 NULL 则恢复使用系统时间
 * @return ESP_OK 成功
 */
esp_err_t logger_set_time_source(void (*get_time)(struct tm *));

/**
 * @brief 反初始化日志系统，关闭文件
 *
 * @return ESP_OK 成功
 */
esp_err_t logger_deinit(void);

#ifdef __cplusplus
}
#endif
