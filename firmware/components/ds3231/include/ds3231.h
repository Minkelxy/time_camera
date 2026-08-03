#pragma once

#include <stdint.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DS3231 I2C 设备地址（7-bit）
 */
#define DS3231_I2C_ADDR 0x68

/**
 * @brief 初始化 DS3231 RTC
 *
 * 配置 I2C 主机并连接 DS3231，校验器件应答。
 *
 * @param port I2C 端口号
 * @param sda  I2C SDA 引脚
 * @param scl  I2C SCL 引脚
 * @return ESP_OK 成功；否则返回错误码
 */
esp_err_t ds3231_init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl);

/**
 * @brief 读取当前时间
 *
 * @param time 输出参数，填充标准 struct tm
 * @return ESP_OK 成功
 */
esp_err_t ds3231_get_time(struct tm *time);

/**
 * @brief 设置时间
 *
 * @param time 待写入的时间
 * @return ESP_OK 成功
 */
esp_err_t ds3231_set_time(const struct tm *time);

/**
 * @brief 设置定时唤醒闹钟
 *
 * 配置 DS3231 闹钟 1 为"每分钟周期触发"或按 minute_interval 分钟触发，
 * INT/SQW 引脚输出低电平脉冲用于唤醒 ESP32-S3。
 *
 * @param minute_interval 唤醒间隔（分钟）
 * @return ESP_OK 成功
 */
esp_err_t ds3231_set_alarm(uint8_t minute_interval);

/**
 * @brief 清除闹钟标志位
 *
 * 唤醒后必须调用，否则 INT 引脚保持低电平无法再次唤醒。
 *
 * @return ESP_OK 成功
 */
esp_err_t ds3231_clear_alarm(void);

/**
 * @brief 读取 DS3231 内置温度传感器
 *
 * @param temp 输出温度（摄氏度）
 * @return ESP_OK 成功
 */
esp_err_t ds3231_get_temperature(float *temp);

#ifdef __cplusplus
}
#endif
