# 户外长期独立运行定时拍照项目 Spec

## Why
在野外监测、生态观察、工地进度记录、风景延时摄影等场景中，需要一套能在户外无市电、无人值守条件下长期（数周到数月）独立运行，并能定时自动拍照、本地存储和（可选）远程回传的嵌入式系统。市面上的方案要么依赖市电、要么功耗高、要么不可定制，因此需要自行规划一套低功耗、可扩展、易维护的软硬件方案。

## What Changes
- 新增硬件方案设计：主控 MCU、摄像模组、RTC、电源（太阳能 + 电池）、存储、外壳防护
- 新增电源管理子系统：低功耗设计、太阳能充电、电池电量监测、深度睡眠
- 新增固件软件方案：基于 ESP-IDF / Arduino 框架，实现定时唤醒、拍照、SD 卡存储、配置管理
- 新增可选通信能力：WiFi/LoRa 回传、OTA 升级
- 新增配置与运维方案：Web 配置门户、配置文件、日志、远程调试
- 新增测试与部署方案：功耗测试、户外长期运行验证、固件烧录流程

## Impact
- Affected specs: 无（首次创建）
- Affected code:
  - `firmware/` 固件源码（主程序、驱动、电源管理、相机控制、存储、通信）
  - `hardware/` 硬件设计文档、原理图、BOM、外壳设计
  - `docs/` 部署、运维、测试文档
  - `tools/` 烧录脚本、配置工具

## ADDED Requirements

### Requirement: 主控与摄像模组选型
系统 SHALL 采用低功耗 MCU 作为主控，并配套摄像头模组完成图像采集。

#### Scenario: 主控选型
- **WHEN** 进行硬件选型
- **THEN** 采用 ESP32 系列（推荐 ESP32-S3 或 ESP32-CAM）作为主控，支持深度睡眠（< 1mA）、双核、充足 GPIO 与外设接口
- **AND** 摄像头采用 OV2640（ESP32-CAM 板载）或 OV5640，分辨率 ≥ 2MP

#### Scenario: 接口需求
- **WHEN** 进行接口规划
- **THEN** 主控至少提供：SD 卡接口（SPI/SDIO）、I2C（接 RTC 与传感器）、UART（调试）、GPIO（控制电源使能）

### Requirement: RTC 定时唤醒
系统 SHALL 使用外接高精度 RTC（DS3231）实现定时唤醒，避免依赖 MCU 内部低精度时钟。

#### Scenario: 定时拍照
- **WHEN** 用户配置拍照间隔（如每 30 分钟一次）
- **THEN** MCU 进入深度睡眠，RTC 在到达设定时刻通过 INT 引脚唤醒 MCU
- **AND** 唤醒后执行：上电摄像头 → 初始化 → 拍照 → 保存 → 关闭摄像头 → 进入深度睡眠

#### Scenario: RTC 掉电保持
- **WHEN** 主电源中断
- **THEN** RTC 通过板载 CR2032 纽扣电池保持时间至少 1 年

### Requirement: 电源管理（太阳能 + 电池）
系统 SHALL 采用太阳能板 + 可充电电池组合供电，支持户外长期独立运行。

#### Scenario: 供电拓扑
- **WHEN** 设计供电链路
- **THEN** 采用 5V/6V 太阳能板 → MPPT/线性充电控制器 → 电池 → LDO/DC-DC → 系统供电
- **AND** 电池优先选用 LiFePO4（18650，3.2V，安全、宽温）或 Li-ion（带保护板）
- **AND** 充电控制器建议 CN3791（MPPT，LiFePO4）或 TP4056（Li-ion）

#### Scenario: 低功耗运行
- **WHEN** 系统处于待机状态
- **THEN** 通过以下手段降低功耗：
  - MCU 进入深度睡眠模式（< 1mA）
  - 摄像头、SD 卡、传感器通过 MOSFET 受控上电，待机时断电
  - 关闭 WiFi/BLE（仅在需要回传时开启）

#### Scenario: 电池电量监测
- **WHEN** 系统运行
- **THEN** 通过 ADC 分压采样电池电压，估算电量，并在低电量时降低拍照频率或停止拍照以保护电池

### Requirement: 本地存储
系统 SHALL 将拍摄的照片存储到 SD 卡，并按日期分目录管理。

#### Scenario: 存储格式
- **WHEN** 拍摄一张照片
- **THEN** 以 JPEG 格式保存至 `/photos/YYYYMMDD/HHMMSS.jpg`
- **AND** 文件名包含时间戳，便于排序与检索

#### Scenario: 存储容量管理
- **WHEN** SD 卡剩余空间低于阈值（如 10%）
- **THEN** 自动清理最早的照片目录，保证新照片可写入

### Requirement: 固件主程序
系统 SHALL 提供基于 ESP-IDF（或 Arduino-ESP32）的固件，实现主控逻辑。

#### Scenario: 启动流程
- **WHEN** MCU 上电或被 RTC 唤醒
- **THEN** 执行：初始化外设 → 读取配置 → 读取 RTC 时间 → 上电摄像头 → 拍照 → 写入 SD 卡 → 关闭摄像头 → （可选）触发回传 → 进入深度睡眠

#### Scenario: 配置读取
- **WHEN** 系统启动
- **THEN** 从 SD 卡根目录的 `config.json` 读取配置（拍照间隔、分辨率、质量、回传开关等）
- **AND** 若配置不存在或损坏，使用默认配置并记录日志

#### Scenario: 错误处理
- **WHEN** 拍照失败、SD 卡写入失败或摄像头初始化失败
- **THEN** 重试 3 次，仍失败则在日志中记录错误码，并进入深度睡眠等待下一次唤醒

### Requirement: 日志系统
系统 SHALL 提供本地日志记录能力，便于故障排查。

#### Scenario: 日志写入
- **WHEN** 系统执行关键操作或发生错误
- **THEN** 将日志写入 SD 卡 `/logs/YYYYMMDD.log`，包含时间戳、事件类型、错误码
- **AND** 单个日志文件超过 1MB 时自动滚动

### Requirement: 可选远程回传
系统 SHALL 可选支持通过 WiFi（或 LoRa）将照片或缩略图回传到服务器。

#### Scenario: WiFi 回传
- **WHEN** 配置开启 WiFi 回传
- **THEN** 系统在拍照后唤醒 WiFi，连接 AP，通过 HTTP POST/MQTT 将照片（或缩略图）上传到指定服务器
- **AND** 上传成功后可选择删除本地原图或保留
- **AND** 上传失败时重试 2 次，仍失败则跳过，下次唤醒再试

### Requirement: 配置与运维
系统 SHALL 提供配置管理入口，便于现场调整参数。

#### Scenario: 配置文件
- **WHEN** 用户需要修改配置
- **THEN** 通过编辑 SD 卡 `config.json` 实现，支持字段：
  - `interval_minutes`: 拍照间隔（分钟）
  - `resolution`: 分辨率（UXGA/SXGA/VGA）
  - `quality`: JPEG 质量（1-63）
  - `upload_enabled`: 是否回传
  - `wifi_ssid` / `wifi_password`: WiFi 凭据
  - `server_url`: 上传服务器地址

#### Scenario: Web 配置门户（可选）
- **WHEN** 设备首次使用或需要现场配置
- **THEN** 长按配置按钮 5 秒进入 AP 模式，提供 Web 页面供用户配置参数，配置保存到 SD 卡

### Requirement: 外壳与防护
系统 SHALL 提供满足户外长期运行的外壳防护设计。

#### Scenario: 防护等级
- **WHEN** 设计外壳
- **THEN** 外壳防护等级 ≥ IP65（防尘防水），摄像头开窗采用光学玻璃 + 密封圈
- **AND** 太阳能板外置或顶部开窗，便于采光

#### Scenario: 散热与温度
- **WHEN** 户外高温或低温环境
- **THEN** 选用宽温元器件（-20℃ ~ +60℃），LiFePO4 电池可在 -10℃ 充电
- **AND** 外壳设计通风散热，避免阳光直射 MCU

### Requirement: 测试与验证
系统 SHALL 经过测试验证满足户外长期运行要求。

#### Scenario: 功耗测试
- **WHEN** 进行功耗测试
- **THEN** 测量深度睡眠电流 < 1mA，工作电流（拍照瞬间）< 300mA，平均功耗满足太阳能供电预算

#### Scenario: 户外长期测试
- **WHEN** 部署到户外
- **THEN** 连续运行 ≥ 7 天，验证：定时拍照正常、SD 卡存储正常、电量维持稳定、无重启异常

## MODIFIED Requirements
无（首次创建）

## REMOVED Requirements
无（首次创建）
