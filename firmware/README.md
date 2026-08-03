# 户外定时拍照固件 (ESP32-S3)

面向户外长期独立运行的定时拍照项目，基于 **ESP32-S3** 与 **ESP-IDF v5.1**。

## 目录结构

```
firmware/
├── CMakeLists.txt          # 顶层 CMake
├── sdkconfig.defaults      # 默认配置 (PSRAM 8MB / Flash 4MB / FATFS / WiFi 等)
├── partitions.csv          # 分区表 (nvs / phy / factory / storage)
├── README.md
├── main/                   # 程序入口 app_main()
└── components/
    ├── app/                # 应用主业务循环
    ├── ds3231/             # DS3231 RTC 驱动 (定时唤醒)
    ├── camera_hal/         # 摄像头 HAL
    ├── sd_storage/         # SD 卡 FATFS 存储
    ├── power_mgmt/         # 电源管理 / 深度睡眠
    ├── logger/             # 日志系统
    ├── config_mgr/         # 配置管理 (/config.json)
    ├── wifi_upload/        # WiFi 回传
    └── config_portal/      # Web 配置门户 (AP + HTTP Server)
```

## 启动流程

1. `app_main()` 打印启动日志
2. `power_mgmt_init()` 初始化电源与 ADC
3. `logger_init()` 初始化日志
4. `config_mgr_load()` 从 SD 卡加载 `/config.json`
5. `app_run()` 进入主循环：上电外设 → 拍照 → 存储 → (可选)回传 → 关外设 → 深度睡眠 → RTC 闹钟唤醒

## 构建与烧录

> 当前为骨架阶段，各组件均为占位实现（`ESP_LOGI` + `TODO`），可独立编译通过。

```bash
# 设置目标芯片（首次）
idf.py set-target esp32s3

# 配置（可选，默认值已写入 sdkconfig.defaults）
idf.py menuconfig

# 编译
idf.py build

# 烧录（替换为实际端口）
idf.py -p /dev/ttyUSB0 -b 921600 flash

# 监视串口
idf.py -p /dev/ttyUSB0 monitor
```

## 分区表

| 名称       | 类型 | 偏移      | 大小      | 说明            |
|------------|------|-----------|-----------|-----------------|
| nvs        | data | 0x9000    | 0x6000    | NVS             |
| phy_init   | data | 0xf000    | 0x1000    | PHY 校准        |
| factory    | app  | 0x10000   | 0x180000  | 应用 (1.5MB)    |
| storage    | data | 0x190000  | 0x270000  | FAT (~2.4MB)    |

## 后续任务

各组件目前为占位实现，需按以下顺序填充：

- [ ] `power_mgmt`：ADC 电池采样、外设电源 MOS 控制、深度睡眠
- [ ] `ds3231`：I2C 读写、闹钟定时唤醒
- [ ] `sd_storage`：SDMMC 挂载 FATFS、文件读写
- [ ] `camera_hal`：通过 `espressif/esp_camera` 组件拍照
- [ ] `logger`：SD 卡日志轮转
- [ ] `config_mgr`：cJSON 解析 `/config.json`
- [ ] `wifi_upload`：HTTP POST 上传照片
- [ ] `config_portal`：SoftAP + HTTP Server 配置页
- [ ] `app`：串联完整业务流程
