# Tasks

## 阶段一：硬件方案设计

- [x] Task 1: 完成硬件选型与系统框图设计
  - [x] SubTask 1.1: 输出主控、摄像头、RTC、电源、存储、传感器选型清单（BOM）→ hardware/BOM.md
  - [x] SubTask 1.2: 绘制系统功能框图（电源链路、信号链路、控制链路）→ hardware/system_architecture.md
  - [x] SubTask 1.3: 输出 GPIO / 接口分配表 → hardware/gpio_allocation.md

- [x] Task 2: 完成电源管理子系统设计 → hardware/power_design.md
  - [x] SubTask 2.1: 设计太阳能板 + 充电控制器 + 电池拓扑（含 MPPT/线性充电选型）
  - [x] SubTask 2.2: 设计 LDO/DC-DC 稳压电路（3.3V 系统供电）
  - [x] SubTask 2.3: 设计受控电源开关电路（MOSFET 控制摄像头/SD/传感器上电）
  - [x] SubTask 2.4: 设计电池电压 ADC 采样分压电路

- [x] Task 3: 完成外壳与防护设计 → hardware/enclosure_design.md
  - [x] SubTask 3.1: 输出外壳结构方案（材料、IP65 防护、摄像头开窗）
  - [x] SubTask 3.2: 输出散热与温度应对方案（防晒、通风、宽温选型）
  - [x] SubTask 3.3: 输出太阳能板安装位置方案

## 阶段二：固件软件实现

- [x] Task 4: 搭建固件工程骨架 → firmware/
  - [x] SubTask 4.1: 创建 ESP-IDF 工程目录结构（firmware/main、firmware/components）
  - [x] SubTask 4.2: 配置构建系统（CMake）与烧录脚本（sdkconfig.defaults、partitions.csv）
  - [x] SubTask 4.3: 实现基础日志输出与看门狗

- [x] Task 5: 实现外设驱动层
  - [x] SubTask 5.1: 实现 DS3231 RTC 驱动（I2C 读写时间、闹钟设置、INT 唤醒）→ components/ds3231/
  - [x] SubTask 5.2: 实现 OV2640 摄像头驱动（初始化、分辨率/质量设置、拍照取帧）→ components/camera_hal/
  - [x] SubTask 5.3: 实现 SD 卡驱动（挂载、文件读写、目录管理）→ components/sd_storage/
  - [x] SubTask 5.4: 实现 ADC 电池电压采样驱动 → components/power_mgmt/power_mgmt.c

- [x] Task 6: 实现电源管理逻辑 → components/power_mgmt/
  - [x] SubTask 6.1: 实现受控外设上电/断电函数（MOSFET 控制）
  - [x] SubTask 6.2: 实现深度睡眠进入与 RTC 唤醒配置（EXT0 GPIO21）
  - [x] SubTask 6.3: 实现低电量保护策略（降频/停拍）

- [x] Task 7: 实现主业务流程 → components/app/app.c、main/main.c
  - [x] SubTask 7.1: 实现配置加载模块（解析 SD 卡 config.json，含默认值回退）→ components/config_mgr/
  - [x] SubTask 7.2: 实现拍照流程：上电 → 初始化 → 取帧 → 写 JPEG → 断电
  - [x] SubTask 7.3: 实现照片存储路径与命名规则 /sdcard/photos/YYYYMMDD/HHMMSS.jpg
  - [x] SubTask 7.4: 实现 SD 卡容量管理（低于 10% 清理最早目录）
  - [x] SubTask 7.5: 实现错误重试（3 次）与日志记录逻辑

- [x] Task 8: 实现日志系统 → components/logger/
  - [x] SubTask 8.1: 实现日志写入 /sdcard/logs/YYYYMMDD.log，含时间戳/事件/错误码
  - [x] SubTask 8.2: 实现日志文件滚动（> 1MB 自动新建）

- [x] Task 9: 实现可选 WiFi 回传模块 → components/wifi_upload/
  - [x] SubTask 9.1: 实现 WiFi 连接管理（按需开启，回传后关闭）
  - [x] SubTask 9.2: 实现 HTTP POST 上传照片逻辑（流式写入，30s 超时）
  - [x] SubTask 9.3: 实现上传失败重试（2 次）与本地保留策略

- [x] Task 10: 实现配置与运维入口 → components/config_portal/
  - [x] SubTask 10.1: 实现 config.json 字段定义与解析 → components/config_mgr/
  - [x] SubTask 10.2: 实现 Web 配置门户（AP 模式 + Web 表单，长按 GPIO0 5 秒触发）

## 阶段三：测试与部署

- [x] Task 11: 实现测试与验证方案 → docs/test_plan.md
  - [x] SubTask 11.1: 编写功耗测试方案，测量深度睡眠/工作电流，验证满足预算
  - [x] SubTask 11.2: 进行室内功能联调方案（定时拍照、存储、配置、日志，15 项测试）
  - [x] SubTask 11.3: 进行户外 7 天长期运行验证方案，输出测试报告模板

- [x] Task 12: 实现部署与运维文档 → docs/
  - [x] SubTask 12.1: 输出固件烧录流程文档 → docs/flashing_guide.md
  - [x] SubTask 12.2: 输出现场部署指南（选址、安装、初次配置）→ docs/deployment_guide.md
  - [x] SubTask 12.3: 输出故障排查指南（日志解读、常见问题）→ docs/troubleshooting.md

# Task Dependencies
- Task 2 依赖 Task 1（电源设计需基于选型）
- Task 3 依赖 Task 1、Task 2（外壳需适配硬件与电源）
- Task 5 依赖 Task 4（驱动需在工程骨架上开发）
- Task 6 依赖 Task 5（电源管理需调用驱动）
- Task 7 依赖 Task 5、Task 6（业务流程依赖驱动与电源管理）
- Task 8 可与 Task 7 并行
- Task 9 依赖 Task 7（回传在拍照后触发）
- Task 10 依赖 Task 7（配置模块）
- Task 11 依赖 Task 2、3、7、8、9、10（测试需软硬件就绪）
- Task 12 依赖 Task 11（文档基于验证结果）
