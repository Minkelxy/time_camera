# 固件烧录流程指南 - 户外定时拍照系统

> 适用项目：基于 ESP32-S3 的太阳能供电户外长期独立运行定时拍照系统
> 目标芯片：ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB Octal PSRAM）
> 固件框架：ESP-IDF v5.1
> 文档版本：v1.0

本文档面向固件开发、生产预烧与现场维护人员，描述从环境搭建到固件烧录、监控、首次配置的完整流程。

---

## 1. 环境准备

### 1.1 操作系统要求

| 操作系统 | 版本要求 | 备注 |
|----------|----------|------|
| Windows | Windows 10/11 64 位 | 需安装 CP210x / CH340 USB 转串口驱动（若使用 USB-TTL 桥）；ESP32-S3 原生 USB 免驱 |
| macOS | macOS 11 (Big Sur) 及以上 | 原生支持 USB，无需额外驱动 |
| Linux | Ubuntu 20.04 / Debian 11 及以上 | 需将用户加入 `dialout` 组以访问串口设备 |

> **Linux 串口权限**：执行 `sudo usermod -aG dialout $USER` 后**重新登录**生效，否则会报 `permission denied: /dev/ttyUSB0`。

### 1.2 ESP-IDF 安装

本项目基于 **ESP-IDF v5.1**，须严格匹配大版本。

#### 方式一：官方一键安装器（推荐，Windows/macOS）

1. 访问 https://dl.espressif.com/dl/esp-idf/ 下载对应系统的 ESP-IDF Tools Installer。
2. 运行安装器，选择 ESP-IDF v5.1.x（最新 5.1 补丁版本）。
3. 安装器会自动配置 Python 3.8+、CMake、Ninja、交叉编译工具链与环境变量。

#### 方式二：命令行手动安装（Linux/macOS）

```bash
# 1. 安装系统依赖（Ubuntu/Debian）
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
    python3-venv cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0

# 2. 克隆 ESP-IDF v5.1（--recursive 拉取子模块）
mkdir -p ~/esp && cd ~/esp
git clone -b v5.1.x --recursive https://github.com/espressif/esp-idf.git
# 注：v5.1.x 会拉取 5.1 系列最新稳定标签

# 3. 安装工具链（Python 依赖 + 交叉编译器）
cd ~/esp/esp-idf
./install.sh esp32s3

# 4. 激活环境变量（每个新终端执行，或写入 ~/.bashrc / ~/.zshrc）
. ~/esp/esp-idf/export.sh
```

#### 验证安装

```bash
idf.py --version
# 应输出 IDF v5.1.x 版本号
```

### 1.3 Python 要求

- 版本：Python 3.8 及以上（ESP-IDF v5.1 不再支持 Python 3.6/3.7）。
- ESP-IDF 安装器会自动创建独立虚拟环境；手动安装时 `install.sh` 会通过 pip 安装所需依赖（`cryptography`、`construct` 等）。

### 1.4 USB 驱动

ESP32-S3-WROOM-1 模组原生支持 **USB-OTG**（GPIO 19/20），可直接通过 USB 数据线烧录与调试，无需外部 USB-UART 桥芯片。

| 连接方式 | 驱动需求 | 端口示例 |
|----------|----------|----------|
| 原生 USB（GPIO19/20，推荐） | 免驱（CDC） | Linux: `/dev/ttyACM0`；macOS: `/dev/cu.usbmodem*`；Windows: `COMx` |
| 外接 USB-TTL 桥（CP2102） | 需 CP210x 驱动 | Linux: `/dev/ttyUSB0`；Windows: `COMx` |
| 外接 USB-TTL 桥（CH340） | 需 CH340 驱动 | 同上 |

> **线缆注意**：必须使用支持**数据传输**的 USB 线（非纯充电线），否则无法识别设备。若插入后无任何反应，首先更换 USB 线排查。

---

## 2. 获取源码

### 2.1 克隆项目仓库

```bash
git clone <项目仓库地址> outdoor-camera
cd outdoor-camera
```

### 2.2 项目目录结构

```
outdoor-camera/
├── firmware/                    # 固件源码
│   ├── CMakeLists.txt           # 顶层 CMake
│   ├── sdkconfig.defaults       # 默认配置（PSRAM/Flash/FATFS/WiFi 等）
│   ├── partitions.csv           # 分区表（nvs/phy/factory/storage）
│   ├── README.md                # 固件构建说明
│   ├── main/                    # 程序入口 app_main()
│   │   └── main.c
│   └── components/
│       ├── app/                 # 应用主业务循环（拍照→存储→回传→睡眠）
│       ├── camera_hal/          # 摄像头 HAL（OV2640 DVP）
│       ├── config_mgr/          # 配置管理（/sdcard/config.json）
│       ├── config_portal/       # Web 配置门户（SoftAP + HTTP Server）
│       ├── ds3231/              # DS3231 RTC 驱动（定时唤醒）
│       ├── logger/              # SD 卡日志系统
│       ├── power_mgmt/          # 电源管理 / 深度睡眠 / ADC 采样
│       ├── sd_storage/          # SD 卡 FATFS 存储
│       └── wifi_upload/         # WiFi 回传
├── hardware/                    # 硬件设计文档
│   ├── BOM.md                   # 物料清单
│   ├── gpio_allocation.md       # GPIO 引脚分配
│   └── enclosure_design.md      # 外壳与防护设计
└── docs/                        # 部署与运维文档
    ├── flashing_guide.md        # 本文档
    ├── deployment_guide.md      # 现场部署指南
    └── troubleshooting.md       # 故障排查指南
```

> 烧录前需进入 `firmware/` 目录执行所有 `idf.py` 命令。

---

## 3. 配置目标芯片

首次编译前必须设置目标芯片为 ESP32-S3：

```bash
cd firmware
idf.py set-target esp32s3
```

此命令会根据 `sdkconfig.defaults` 生成 `sdkconfig` 文件，并配置工具链指向 Xtensa ESP32-S3 编译器。

> 若切换了 IDF 版本或修改了 `sdkconfig.defaults`，删除 `build/` 目录后重新执行 `set-target`。

---

## 4. 配置 sdkconfig

项目已提供 `sdkconfig.defaults`，开箱即用。如需调整，执行：

```bash
idf.py menuconfig
```

### 4.1 关键配置项说明

| 配置项 | 默认值 | menuconfig 路径 | 说明 |
|--------|--------|-----------------|------|
| `CONFIG_IDF_TARGET` | `esp32s3` | — | 目标芯片，由 set-target 设置 |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `4MB` | Serial flasher config → Flash size | Flash 大小。模组硬件为 16MB，固件默认按 4MB 配置；如需更大 app 分区可改为 16MB 并同步修改 partitions.csv |
| `CONFIG_ESPTOOLPY_FLASHMODE` | DIO | Serial flasher config → Flash mode | 双线 IO，兼顾速度与稳定性 |
| `CONFIG_ESPTOOLPY_FLASHFREQ` | 40MHz | Serial flasher config → Flash frequency | Flash 时钟频率 |
| `CONFIG_ESP32S3_SPIRAM_SUPPORT` | `y` | Component config → ESP32S3-Specific → Support for SPI PSRAM | 必须开启，OV2640 UXGA JPEG 缓冲依赖 PSRAM |
| `CONFIG_SPIRAM_TYPE` | Quad | Component config → SPI RAM → SPI RAM config | PSRAM 类型（Octal 模组选 Octal，Quad 模组选 Quad） |
| `CONFIG_SPIRAM_SIZE` | 8388608 (8MB) | 同上 | PSRAM 容量，N16R8 模组为 8MB |
| `CONFIG_SPIRAM_SPEED` | 64MHz | 同上 | PSRAM 时钟 |
| `CONFIG_SPIRAM_USE_MALLOC` | `y` | 同上 | 允许 malloc 分配 PSRAM |
| `CONFIG_FATFS_SUPPORT` | `y` | Component config → FAT Filesystem Support | SD 卡 FATFS 支持，必须开启 |
| `CONFIG_FATFS_CODEPAGE` | 936 | 同上 | 简体中文代码页 |
| `CONFIG_ESP_WIFI_ENABLED` | `y` | Component config → Wi-Fi | WiFi 回传与配置门户依赖 |
| `CONFIG_ESP_WIFI_SOFTAP_SUPPORT` | `y` | 同上 | 配置门户 SoftAP 模式 |
| `CONFIG_BT` | `n` | Component config → Bluetooth | 关闭蓝牙以减小体积、降低功耗 |
| `CONFIG_ESP_TASK_WDT` | `n` | Component config → FreeRTOS → Task Watchdog | 关闭任务看门狗，避免拍照阻塞触发复位 |
| `CONFIG_LOG_DEFAULT_LEVEL` | 3 (INFO) | Component config → Log output → Default log verbosity | 默认日志级别 |

### 4.2 PSRAM 类型注意事项

- **N16R8 模组**（本项目 BOM 指定）：Octal PSRAM，`CONFIG_SPIRAM_TYPE` 应设为 **Octal**。
- **N8R2 模组**（备选）：Quad PSRAM，`CONFIG_SPIRAM_TYPE` 应设为 **Quad**。

> 若 PSRAM 类型不匹配，运行时会因 PSRAM 初始化失败导致摄像头无法分配帧缓冲，表现为拍照失败或系统重启。

---

## 5. 编译

```bash
idf.py build
```

编译产物位于 `firmware/build/`，关键文件：
- `firmware.bin`：应用固件（烧录到 factory 分区 0x10000）
- `bootloader/bootloader.bin`：引导加载程序（烧录到 0x0）
- `partition_table/partition-table.bin`：分区表（烧录到 0x8000）

### 5.1 常见编译错误及解决

| 错误现象 | 可能原因 | 解决方法 |
|----------|----------|----------|
| `idf.py: command not found` | 环境变量未激活 | 执行 `. $IDF_PATH/export.sh` |
| `Unsupported IDF version` | IDF 版本非 5.1 | 安装 ESP-IDF v5.1.x |
| `SPIRAM init failed` 编译报错 | PSRAM 配置与模组不符 | menuconfig 中修改 SPIRAM 类型（Octal/Quad） |
| `region 'dram' overflowed` | 固件体积超分区 | 减小 app 分区占用，或改 Flash 为 16MB 并扩大 factory 分区 |
| `cJSON` / `esp_camera` 找不到 | 组件依赖未拉取 | 确认 `components/` 目录完整，必要时 `idf.py reconfigure` |
| Python 包导入错误 | Python 环境损坏 | 删除 `~/esp/esp-idf/tools` 后重新执行 `./install.sh esp32s3` |
| `cmake` / `ninja` 未找到 | 构建工具未安装 | Linux 用 apt 安装，或使用官方安装器 |

---

## 6. 烧录

### 6.1 硬件连接

1. 用 **USB 数据线**（非纯充电线）连接 ESP32-S3 开发板/模组的 USB 口与电脑。
2. 确认电脑识别到串口设备：
   - Linux: `ls /dev/ttyUSB* /dev/ttyACM*`
   - macOS: `ls /dev/cu.usb*`
   - Windows: 设备管理器 → 端口（COM 和 LPT）

### 6.2 进入下载模式

ESP32-S3 进入下载（Download Boot）模式有两种方式：

#### 方式一：自动下载（推荐）

ESP32-S3 原生 USB 与多数开发板的 USB-TTL 桥（CP2102/CH340）均带有自动复位电路（DTR/RTS 控制 EN 与 GPIO0）。直接执行 `idf.py flash`，工具会自动复位芯片进入下载模式，无需手动按键。

#### 方式二：手动 BOOT + RST 按键组合

若自动下载失败（无自动复位电路，或 GPIO0 被外设占用），手动操作：

1. 按住 **BOOT** 键（即 GPIO0，对应本项目的配置按钮）不松开。
2. 按一下 **RST**（EN）键后松开。
3. 松开 **BOOT** 键。

此时芯片进入下载模式，串口可被 esptool 识别。

> **本项目设计提示**：GPIO0 同时是配置按钮。按住 GPIO0 + 复位 = 进入下载模式，这是有意设计的**恢复烧录入口**，即使固件异常也可重新烧录。

### 6.3 烧录命令

```bash
# Linux / macOS（原生 USB 多为 ttyACM0，USB-TTL 桥为 ttyUSB0）
idf.py -p /dev/ttyACM0 -b 921600 flash

# Windows（COM 口号从设备管理器查看，示例 COM3）
idf.py -p COM3 -b 921600 flash
```

参数说明：
- `-p`：指定串口设备。
- `-b 921600`：烧录波特率，高速烧录（默认 460800，可降为 115200 提升稳定性）。

`idf.py flash` 会自动烧录 bootloader、partition-table、firmware 三个镜像到对应地址，并支持增量烧录（仅烧录有变化的镜像）。

### 6.4 烧录地址与分区表

项目使用自定义分区表 `partitions.csv`：

| 名称 | 类型 | 偏移地址 | 大小 | 说明 |
|------|------|----------|------|------|
| nvs | data (nvs) | 0x9000 | 0x6000 (24KB) | 非易失存储（WiFi 凭据等） |
| phy_init | data (phy) | 0xf000 | 0x1000 (4KB) | RF PHY 校准数据 |
| factory | app (factory) | 0x10000 | 0x180000 (1.5MB) | 应用固件 |
| storage | data (fat) | 0x190000 | 0x270000 (2.4MB) | FAT 数据分区（内置存储，照片/日志另存 SD 卡） |

引导加载程序烧录地址：`0x0`；分区表烧录地址：`0x8000`。

> **照片与日志实际存储在 SD 卡**（`/sdcard/photos/` 与 `/sdcard/logs/`），而非内置 storage 分区。storage 分区为预留扩展用途。

### 6.5 擦除 Flash

以下场景需先擦除整个 Flash：
- **首次烧录**全新芯片。
- 出现**异常行为**（NVS 数据损坏、PHY 校准异常、配置混乱）。
- 切换了**分区表**或 Flash 大小配置。

```bash
idf.py -p /dev/ttyACM0 erase-flash
```

擦除后需重新烧录固件：

```bash
idf.py -p /dev/ttyACM0 flash
```

> `erase-flash` 会清除所有分区数据（含 NVS 中保存的 WiFi 凭据）。擦除后配置门户与 SD 卡上的 config.json 仍可重新配置。

---

## 7. 监控

### 7.1 启动串口监控

```bash
# 烧录后立即监控（推荐）
idf.py -p /dev/ttyACM0 flash monitor

# 仅监控（不烧录）
idf.py -p /dev/ttyACM0 monitor
```

- 退出监控：`Ctrl + ]`
- 监控期间按开发板 RST 键可重启设备并重新输出启动日志。

### 7.2 串口输出说明

串口波特率固定 **115200 8N1**（GPIO43=TX, GPIO44=RX）。关键日志阶段：

| 阶段 | 日志示例 | 含义 |
|------|----------|------|
| 启动 | `户外定时拍照固件启动 (ESP32-S3)` | app_main 入口 |
| 唤醒原因 | `唤醒原因: EXT0 (DS3231 闹钟唤醒)` | RTC 闹钟唤醒；`POWERON` 为首次上电 |
| RTC 时间 | `RTC 时间: 2025-08-03 14:30:00` | DS3231 当前时间 |
| 电池电压 | `电池电压: 3.28V (85%)` | LiFePO4 电压与估算电量 |
| 配置加载 | `配置加载成功：interval=5, resolution=1600x1200...` | 从 SD 卡读取 config.json |
| 拍照成功 | `拍照成功: 245680 字节` | JPEG 帧大小 |
| 照片保存 | `照片已保存: /sdcard/photos/20250803/143000.jpg` | SD 卡存储路径 |
| 上传 | `照片已上传` 或 `上传重试 2 次仍失败` | WiFi 回传结果 |
| 睡眠 | `==== 进入深度睡眠，下次唤醒间隔 5 分钟 ====` | 进入深睡，等待 RTC 唤醒 |

> **正常循环**：每次 RTC 唤醒后系统复位，重新执行 app_main，形成"醒→拍→存→睡"循环。监控中会周期性看到上述日志。

---

## 8. 首次配置

固件烧录完成后，需配置拍照参数（间隔、分辨率、WiFi 回传等）。系统支持两种配置方式。

### 8.1 方式一：SD 卡写入 config.json（推荐）

适用于首次部署、批量预配置，无需现场连接 WiFi。

1. 将 MicroSD 卡格式化为 **FAT32**（SD 卡容量 16~32GB，Class 10 及以上）。
2. 在 SD 卡**根目录**创建 `config.json`，写入配置（模板见下方）。
3. 将 SD 卡插入设备的 MicroSD 卡座。
4. 上电启动，固件自动读取 `/sdcard/config.json` 并应用。

> 若 config.json 不存在或 JSON 格式错误，固件使用**内置默认配置**（间隔 5 分钟、UXGA、质量 10、不回传）并记录日志，不会阻塞启动。

#### config.json 模板

```json
{
  "interval_minutes": 30,
  "resolution": "1600x1200",
  "quality": 10,
  "upload_enabled": false,
  "wifi_ssid": "",
  "wifi_password": "",
  "server_url": ""
}
```

字段说明：

| 字段 | 类型 | 取值范围 | 默认值 | 说明 |
|------|------|----------|--------|------|
| `interval_minutes` | 整数 | 1 ~ 1440 | 5 | 拍照间隔（分钟），1 分钟~24 小时 |
| `resolution` | 字符串 | `1600x1200` / `1280x1024` / `640x480` | `1600x1200` | 分辨率（UXGA/SXGA/VGA） |
| `quality` | 整数 | 4 ~ 63 | 10 | JPEG 质量，**数值越小质量越高**（esp_camera 规范） |
| `upload_enabled` | 布尔 | true / false | false | 是否启用 WiFi 回传 |
| `wifi_ssid` | 字符串 | 最长 63 字符 | `""` | WiFi 网络名称（upload_enabled=true 时必填） |
| `wifi_password` | 字符串 | 最长 63 字符 | `""` | WiFi 密码 |
| `server_url` | 字符串 | 最长 127 字符 | `""` | 照片上传服务器 URL，如 `http://example.com/upload`（upload_enabled=true 时必填） |

> **质量参数说明**：esp_camera 的 quality 参数与常见 JPEG quality 相反——**数值越小，画质越高、文件越大**。推荐 5~15（高质量），20~30（中等），不建议低于 4。

### 8.2 方式二：长按 GPIO0 进入配置门户

适用于现场调试、临时修改参数，无需取出 SD 卡。

1. 设备上电启动。
2. **上电后立即长按配置按钮（GPIO0）持续 5 秒**。
3. 串口日志出现 `启动配置门户（SoftAP + HTTP Server）`。
4. 设备开启 WiFi 热点，SSID 形如 **`OutdoorCam-XXXX`**（XXXX 为设备 MAC 地址后 4 位十六进制）。
5. 用手机或电脑连接该热点：
   - **SSID**：`OutdoorCam-XXXX`
   - **密码**：`12345678`
6. 浏览器访问 **`http://192.168.4.1/`**，打开配置页面。
7. 在表单中填写各项参数，点击"💾 保存并重启"。
8. 设备保存配置到 SD 卡 `/sdcard/config.json`，3 秒后自动重启，进入正常拍照循环。

#### 配置门户状态查询

浏览器访问 `http://192.168.4.1/status` 可获取设备当前状态（JSON 格式），包含：
- 电池电压与电量百分比
- SD 卡剩余空间（MB）
- 当前配置参数

> **超时机制**：配置门户运行 30 分钟无操作自动关闭并退出，设备进入正常流程。保存配置后会立即重启，无需等待超时。

---

## 9. OTA 升级（可选，未来扩展）

当前固件版本**暂不支持 OTA（空中升级）**，所有固件更新通过 USB 重新烧录完成。

未来扩展方向：
- 在 factory 分区之外增加 OTA 数据分区与备用 app 分区，实现 A/B 双区 OTA。
- 通过 WiFi 回传通道下发固件镜像，esp_ota_* API 写入备用分区后切换启动。

> 当前升级流程：USB 连接 → `idf.py flash` 重新烧录（NVS 与 SD 卡数据不受影响，除非执行 `erase-flash`）。

---

## 10. 常见问题

| 现象 | 可能原因 | 解决方法 |
|------|----------|----------|
| 插入 USB 后电脑无反应 | USB 线为纯充电线；或驱动未安装 | 更换支持数据传输的 USB 线；安装 CP210x/CH340 驱动 |
| `A fatal error occurred: Failed to connect to ESP32-S3` | 未进入下载模式；GPIO0 被外设拉低 | 手动 BOOT+RST 进入下载模式；检查 GPIO0 外设是否冲突 |
| 端口被占用（`Permission denied` / `device busy`） | 其他程序占用串口；Linux 用户未在 dialout 组 | 关闭占用程序（如其他监控、Arduino IDE）；Linux 执行 `sudo usermod -aG dialout $USER` 后重新登录 |
| 烧录到一半中断 | USB 线接触不良；供电不足 | 更换 USB 线；使用带独立供电的 USB Hub 或直接接主板 USB 口 |
| 串口无输出 | 波特率不符；TX/RX 接反 | 确认监控波特率为 115200；检查 USB-TTL 桥 TX/RX 接线 |
| 反复重启（boot loop） | Flash 擦除不全；分区表不匹配 | 执行 `idf.py erase-flash` 后重新烧录 |
| `SPIRAM not initialized` | PSRAM 类型配置错误 | menuconfig 修改 SPIRAM 类型（N16R8 选 Octal） |
| 摄像头初始化失败 | PSRAM 未启用；摄像头排线松动 | 确认 `CONFIG_SPIRAM_SUPPORT=y`；检查 FPC 排线插接 |
| 配置门户无法访问 | 手机未连接 OutdoorCam 热点；地址错误 | 确认连接 SSID `OutdoorCam-XXXX`；访问 `http://192.168.4.1/`（非 https） |
| 烧录成功但不拍照 | SD 卡未插入或格式非 FAT32；RTC 故障 | 插入 FAT32 格式 SD 卡；查看串口日志定位错误 |

---

## 附录：烧录快速参考

```bash
# === 一次性流程 ===
cd firmware
. $IDF_PATH/export.sh              # 激活环境（每次新终端）
idf.py set-target esp32s3          # 首次设置目标
idf.py build                       # 编译
idf.py -p /dev/ttyACM0 flash monitor   # 烧录并监控

# === 异常恢复 ===
idf.py -p /dev/ttyACM0 erase-flash # 擦除全 Flash
idf.py -p /dev/ttyACM0 flash       # 重新烧录
```
