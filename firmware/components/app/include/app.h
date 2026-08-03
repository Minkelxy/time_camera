#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用主业务循环
 *
 * 串联完整业务流程：
 *   上电 → 配置 → 拍照 → 存储 → (可选)回传 → 关外设 → 深度睡眠 → RTC 唤醒
 *
 * 该函数在内部循环执行，最终调用 power_enter_deep_sleep() 进入低功耗；
 * RTC 闹钟唤醒后系统复位，重新进入 app_main()，从而形成周期性拍照循环。
 *
 * @return ESP_OK 成功（正常情况下因进入深度睡眠而不会返回）
 */
esp_err_t app_run(void);

#ifdef __cplusplus
}
#endif
