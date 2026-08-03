#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 可控外设枚举
 *
 * 每个外设通过独立 MOS 管控制电源，按需上下电以降低静态功耗。
 */
typedef enum {
    P_CAM = 0,   ///< 摄像头
    P_SD,        ///< SD 卡
    P_SENSOR,    ///< 传感器（如 BME280 等，本项目未连接 MOSFET，仅占位）
    P_MAX        ///< 枚举上界（用于内部校验）
} peripheral_t;

/**
 * @brief 初始化电源管理
 *
 * 配置外设电源控制 GPIO 为输出、ADC 通道用于电池电压采样、
 * 配置按钮输入。默认所有外设处于断电状态。
 *
 * @return ESP_OK 成功
 */
esp_err_t power_mgmt_init(void);

/**
 * @brief 使能指定外设电源
 *
 * P-MOSFET 栅极拉低导通，并在上电后留 100ms 稳定延时。
 *
 * @param p 外设类型
 * @return ESP_OK 成功
 */
esp_err_t power_enable_peripheral(peripheral_t p);

/**
 * @brief 关闭指定外设电源
 *
 * P-MOSFET 栅极拉高关断。
 *
 * @param p 外设类型
 * @return ESP_OK 成功
 */
esp_err_t power_disable_peripheral(peripheral_t p);

/**
 * @brief 读取电池电压
 *
 * 通过 ADC1_CH0 多次采样电池分压电路并换算为实际电压（Vbat = V_sense × 2）。
 * 使用 esp_adc_cal 校准提高精度。
 *
 * @param voltage 输出电压（伏特）
 * @return ESP_OK 成功
 */
esp_err_t power_read_battery_voltage(float *voltage);

/**
 * @brief 根据电压估算电池剩余百分比
 *
 * 采用 LiFePO4 放电曲线分段映射：
 *   >= 3.4V → 100%
 *   3.4~3.2V → 100~50%（线性）
 *   3.2~3.0V → 50~10%（线性）
 *   <= 3.0V → 0%
 *
 * @param voltage 电池电压
 * @return 百分比 [0.0, 100.0]
 */
float power_get_battery_percent(float voltage);

/**
 * @brief 进入深度睡眠
 *
 * 配置 EXT0 唤醒源（DS3231 闹钟中断，GPIO21 低电平触发），
 * 可选 EXT1 唤醒源（wake_gpio，如配置按钮 GPIO0），
 * 并以 timer 作为兜底防 RTC 闹钟失败。唤醒后系统复位重新执行 app_main()。
 *
 * @param seconds   睡眠时长（秒），>0 时启用 timer 兜底唤醒
 * @param wake_gpio 附加 EXT1 唤醒引脚（0 表示不启用附加引脚，仅靠 DS3231 INT）
 * @return 正常情况下不返回（系统复位）
 */
esp_err_t power_enter_deep_sleep(uint32_t seconds, gpio_num_t wake_gpio);

/**
 * @brief 判断电池电压是否处于低电量
 *
 * 阈值 3.0V，低于则需启动省电策略。
 *
 * @param voltage 电池电压
 * @return true 低电量；false 电量正常
 */
bool power_is_battery_low(float voltage);

/**
 * @brief 根据电池电压决定是否跳过本次拍照，并动态调整拍照间隔
 *
 * LiFePO4 低电量保护策略：
 *   >= 3.2V：维持原间隔，正常拍照
 *   3.0~3.2V：间隔翻倍（降低拍照频率，省电）
 *   < 3.0V：跳过拍照，仅维持 RTC + 深睡，等待太阳能回充
 *
 * @param voltage        电池电压
 * @param interval_minutes 输入输出参数：传入原间隔（分钟），函数可能将其翻倍
 * @return true 跳过本次拍照；false 正常拍照（间隔可能被调整）
 */
bool power_should_skip_capture(float voltage, uint8_t *interval_minutes);

#ifdef __cplusplus
}
#endif
