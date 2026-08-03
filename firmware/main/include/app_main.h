#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用主程序入口
 *
 * 由 ESP-IDF 启动系统调用。负责按顺序初始化各子系统并进入主业务循环。
 *
 * 启动流程：
 *   1. 打印启动日志
 *   2. power_mgmt_init()   电源管理初始化
 *   3. logger_init()       日志系统初始化
 *   4. config_mgr_load()   从 SD 卡加载配置
 *   5. app_run()           进入主业务循环
 */
void app_main(void);

#ifdef __cplusplus
}
#endif
