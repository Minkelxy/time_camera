# ESP32-S3 GPIO 分配表 - 户外定时拍照系统

> 文档版本：v1.0
> 主控模组：ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB Octal PSRAM）
> 本文档定义所有 GPIO 的功能分配，作为 PCB 布线与固件 pin 配置的依据。

## 1. ESP32-S3 引脚约束总览

在分配 GPIO 前，必须先了解 ESP32-S3-WROOM-1 模组的引脚约束，避免误用导致启动失败或功能异常。

### 1.1 可用 GPIO 总数

ESP32-S3 芯片共 45 个 GPIO（编号 0~21、26~48，无 22~25）。在 WROOM-1 模组中：

| GPIO 范围 | 模组状态 | 说明 |
|-----------|----------|------|
| GPIO 0~21 | **可用**（22 个） | 全部引出至模组焊盘，含 RTC GPIO（支持深度睡眠唤醒） |
| GPIO 22~25 | 不存在 | ESP32-S3 芯片无此编号 |
| GPIO 26~32 | **不可用** | 模组内部连接 SPI Flash，禁止使用 |
| GPIO 33~37 | **不可用**（N16R8 Octal PSRAM 版本） | 模组内部连接 Octal PSRAM，禁止使用；Quad PSRAM 版本（R2）部分可用 |
| GPIO 38~48 | **可用**（11 个） | 全部引出至模组焊盘 |

> **本模组（N16R8）实际可用 GPIO = 22 + 11 = 33 个**

### 1.2 Strapping 引脚（启动配置，慎用）

以下引脚在 **上电复位与复位释放瞬间** 的电平决定芯片启动模式，必须通过电阻固定到安全电平，**严禁在启动时被外部强下拉/上拉**：

| 引脚 | 默认要求 | 复位时电平 | 误用后果 |
|------|----------|-----------|----------|
| GPIO 0 | HIGH = Flash Boot（正常启动）；LOW = Download Boot（烧录模式） | 必须 HIGH | 启动进入下载模式，无法运行固件 |
| GPIO 3 | HIGH = JTAG 信号来自 IO MUX；LOW = JTAG 信号来自 GPIO 矩阵 | 一般 HIGH | 影响 JTAG 调试路由 |
| GPIO 45 | HIGH = VDD_SPI 由内部 LDO 提供 3.3V；LOW = 1.8V | 必须 HIGH（WROOM-1 用 3.3V Flash） | Flash 电压错误，无法启动 |
| GPIO 46 | HIGH = 启动时打印 ROM 消息；LOW = 禁止 | 一般 HIGH | 影响 ROM 日志输出 |

> **本项目 GPIO 0 用于配置按钮**（带 10kΩ 上拉，按下接地）。复位时按钮未按下 = HIGH = 正常启动；若按下复位 = LOW = 进入下载模式（**作为恢复烧录入口，是有意设计**）。

### 1.3 USB 引脚（占用则失去原生 USB）

| 引脚 | 功能 | 本项目处理 |
|------|------|-----------|
| GPIO 19 | USB_D-（原生 USB-OTG） | **保留不分配**，用于 USB 烧录/调试 |
| GPIO 20 | USB_D+（原生 USB-OTG） | **保留不分配**，用于 USB 烧录/调试 |

### 1.4 Input-Only 引脚

> **ESP32-S3 没有 input-only 引脚**（区别于经典 ESP32 的 GPIO 34~39）。所有可用 GPIO 均为双向，可作输入或输出。

### 1.5 RTC GPIO（支持深度睡眠唤醒）

ESP32-S3 的 **GPIO 0~21 全部为 RTC GPIO**（RTC_GPIO0~21），支持 EXT0/EXT1/触摸唤醒。GPIO 38~48 **不是 RTC GPIO**，深度睡眠期间不可作唤醒源。

本项目唤醒源（DS3231 INT、配置按钮）必须分配在 GPIO 0~21 范围内。

### 1.6 ADC 通道分布

| ADC 单元 | 通道 | 对应 GPIO | 备注 |
|----------|------|-----------|------|
| ADC1 | CH0~CH9 | GPIO 1~10 | **Wi-Fi 工作时仍可使用**，本项目优先使用 |
| ADC2 | CH0~CH9 | GPIO 11~20 | **Wi-Fi 工作时不可用**（被 Wi-Fi 占用），仅在非联网时使用 |

> 电池电压采样需在 Wi-Fi 回传前后都可能读取，**必须使用 ADC1**（GPIO 1~10）。

### 1.7 复用注意事项

- **GPIO 43/44**：默认 UART0（U0TXD/U0RXD），用作调试串口，避免重映射。
- **GPIO 0**：同时是 strapping 引脚 + RTC GPIO + ADC2_CH0，本项目用作配置按钮。
- **PSRAM 占用**：Octal PSRAM 工作时会占用内部 SPI 总线，固件需开启 `CONFIG_SPIRAM`，对 GPIO 26~37 的影响已由模组内部处理，外部不可触碰。

---

## 2. GPIO 分配总表

| 引脚号 | 功能 | 外设/接口 | 方向 | 电平/默认 | 备注 |
|--------|------|-----------|------|-----------|------|
| GPIO 0 | 配置按钮（CONFIG_BTN） | RTC GPIO / EXT1 唤醒 | 输入 | 上拉 HIGH，按下 LOW | ⚠ Strapping 引脚，启动时必须 HIGH；按下复位进入下载模式（恢复烧录用） |
| GPIO 1 | 电池电压采样（VBAT_SENSE） | ADC1_CH0 | 输入 | 模拟 | 分压 100k/100k，Vbat/2 |
| GPIO 2 | 摄像头电源使能（CAM_EN） | GPIO 输出 | 输出 | 上拉 HIGH=断电，LOW=上电 | 驱动 AO3401 Q1 栅极 |
| GPIO 3 | — | — | — | — | ⚠ Strapping（JTAG 路由），不分配，留空 |
| GPIO 4 | 摄像头 SCCB SDA（SIOD） | I2C0 SDA | 双向 | 上拉 | OV2640 寄存器配置，地址 0x30 |
| GPIO 5 | 摄像头 SCCB SCL（SIOC） | I2C0 SCL | 输出 | 上拉 | OV2640 寄存器配置 |
| GPIO 6 | 摄像头 VSYNC | LCD_CAM | 输入 | — | 帧同步 |
| GPIO 7 | 摄像头 HREF（HSYNC） | LCD_CAM | 输入 | — | 行同步 |
| GPIO 8 | 摄像头 Y4（D2） | LCD_CAM | 输入 | — | DVP 数据线 D2 |
| GPIO 9 | 摄像头 Y3（D1） | LCD_CAM | 输入 | — | DVP 数据线 D1 |
| GPIO 10 | 摄像头 Y5（D3） | LCD_CAM | 输入 | — | DVP 数据线 D3 |
| GPIO 11 | 摄像头 Y2（D0） | LCD_CAM | 输入 | — | DVP 数据线 D0 |
| GPIO 12 | 摄像头 Y6（D4） | LCD_CAM | 输入 | — | DVP 数据线 D4 |
| GPIO 13 | 摄像头 PCLK | LCD_CAM | 输入 | — | 像素时钟 |
| GPIO 14 | 摄像头 RESET | GPIO 输出 | 输出 | HIGH=正常，LOW=复位 | 低电平复位 OV2640 |
| GPIO 15 | 摄像头 XCLK | LCD_CAM | 输出 | — | 主时钟输出 16MHz |
| GPIO 16 | 摄像头 Y9（D7） | LCD_CAM | 输入 | — | DVP 数据线 D7 |
| GPIO 17 | 摄像头 Y8（D6） | LCD_CAM | 输入 | — | DVP 数据线 D6 |
| GPIO 18 | 摄像头 Y7（D5） | LCD_CAM | 输入 | — | DVP 数据线 D5 |
| GPIO 19 | USB_D-（保留） | USB-OTG | — | — | 原生 USB，不分配 |
| GPIO 20 | USB_D+（保留） | USB-OTG | — | — | 原生 USB，不分配 |
| GPIO 21 | DS3231 闹钟中断（RTC_INT） | RTC GPIO / EXT0 唤醒 | 输入 | 上拉 HIGH，闹钟时 LOW | ⭐ 核心唤醒源，EXT0 下降沿唤醒 |
| GPIO 38 | SD 卡 SPI MOSI | SPI2 / FSPICSx | 输出 | — | SPI 主出从入 |
| GPIO 39 | SD 卡 SPI MISO | SPI2 | 输入 | — | SPI 主入从出 |
| GPIO 40 | SD 卡 SPI SCK | SPI2 | 输出 | — | SPI 时钟 20MHz |
| GPIO 41 | SD 卡 SPI CS | GPIO 输出 | 输出 | HIGH=未选中，LOW=选中 | 片选 |
| GPIO 42 | SD 卡电源使能（SD_EN） | GPIO 输出 | 输出 | 上拉 HIGH=断电，LOW=上电 | 驱动 AO3401 Q2 栅极 |
| GPIO 43 | UART0 TX（调试） | UART0 | 输出 | — | 115200bps，调试输出 |
| GPIO 44 | UART0 RX（调试） | UART0 | 输入 | — | 调试输入/烧录 |
| GPIO 45 | — | — | — | — | ⚠ Strapping（VDD_SPI 电压），不分配，启动时必须 HIGH |
| GPIO 46 | — | — | — | — | ⚠ Strapping（ROM 日志），不分配，留空 |
| GPIO 47 | 预留扩展（I2C1 SDA / LoRa / 温湿度） | — | 双向 | — | 预留未来扩展（如 SHT30 温湿度传感器） |
| GPIO 48 | 预留扩展（I2C1 SCL / LoRa / 温湿度） | — | 输出 | — | 预留未来扩展 |
| GPIO 22~25 | 不存在 | — | — | — | ESP32-S3 无此引脚 |
| GPIO 26~37 | 模组内部占用 | SPI Flash / Octal PSRAM | — | — | **绝对禁止使用** |

---

## 3. 按接口分组的引脚分配

### 3.1 摄像头 DVP 接口（OV2640）

OV2640 通过 8-bit DVP 并行接口与 ESP32-S3 LCD_CAM 外设连接，配合 SCCB（I2C 兼容）配置寄存器。

| 信号 | ESP32-S3 引脚 | OV2640 引脚 | 方向 | 说明 |
|------|--------------|-------------|------|------|
| D0 (Y2) | GPIO 11 | Y2 | 输入 | 数据位 0 |
| D1 (Y3) | GPIO 9 | Y3 | 输入 | 数据位 1 |
| D2 (Y4) | GPIO 8 | Y4 | 输入 | 数据位 2 |
| D3 (Y5) | GPIO 10 | Y5 | 输入 | 数据位 3 |
| D4 (Y6) | GPIO 12 | Y6 | 输入 | 数据位 4 |
| D5 (Y7) | GPIO 18 | Y7 | 输入 | 数据位 5 |
| D6 (Y8) | GPIO 17 | Y8 | 输入 | 数据位 6 |
| D7 (Y9) | GPIO 16 | Y9 | 输入 | 数据位 7 |
| VSYNC | GPIO 6 | VSYNC | 输入 | 帧同步，垂直消隐 |
| HREF | GPIO 7 | HREF | 输入 | 行有效信号（替代 HSYNC） |
| PCLK | GPIO 13 | PCLK | 输入 | 像素时钟，最高 80MHz |
| XCLK | GPIO 15 | XCLK | 输出 | ESP32 提供 16MHz 主时钟 |
| SIOD (SDA) | GPIO 4 | SIOD | 双向 | SCCB I2C 数据，地址 0x30 |
| SIOC (SCL) | GPIO 5 | SIOC | 输出 | SCCB I2C 时钟 |
| RESET | GPIO 14 | RESET | 输出 | 低电平复位，常态 HIGH |
| PWDN | — | PWDN | — | **不分配**，直接接 GND（常态激活）；电源由 MOSFET Q1 控制 |
| 3.3V VDD | — | 3V3/DVDD/DOVDD/AVDD | 电源 | 经 AO3401 Q1 受控上电 |
| GND | — | GND | 地 | 公共地 |

> **PWDN 处理说明**：OV2640 PWDN 高电平=待机（约 50µA），低电平=激活。本项目通过 AO3401 MOSFET 完全切断电源（0µA，优于 PWDN 待机），故 PWDN 直接接地，不占用 GPIO。

### 3.2 SD 卡 SPI 接口（MicroSD）

| 信号 | ESP32-S3 引脚 | SD 卡引脚 | 方向 | 说明 |
|------|--------------|-----------|------|------|
| MOSI | GPIO 38 | CMD（Pin 2） | 输出 | SPI 主出从入 |
| MISO | GPIO 39 | D0（Pin 7） | 输入 | SPI 主入从出，需 10kΩ 上拉 |
| SCK | GPIO 40 | CLK（Pin 5） | 输出 | SPI 时钟，默认 20MHz |
| CS | GPIO 41 | D3（Pin 1） | 输出 | 片选，低有效，需 10kΩ 上拉 |
| VDD | — | VDD（Pin 4） | 电源 | 经 AO3401 Q2 受控上电，3.3V |
| GND | — | VSS（Pin 3,6） | 地 | 公共地 |

> **SPI 模式说明**：MicroSD 默认支持 SPI 模式（1-bit），无需 SDIO。CS/MISO 各加 10kΩ 上拉确保总线空闲稳定。SD 卡电源受控于 Q2，深度睡眠时断电。

### 3.3 I2C 接口（DS3231 RTC）

| 信号 | ESP32-S3 引脚 | DS3231 引脚 | 方向 | 说明 |
|------|--------------|-------------|------|------|
| SDA | GPIO 4 | SDA（Pin 15） | 双向 | 与摄像头 SCCB 共享 I2C0，DS3231 地址 0x68 |
| SCL | GPIO 5 | SCL（Pin 16） | 输出 | 与摄像头 SCCB 共享 I2C0 |
| INT/SQW | GPIO 21 | INT/SQW（Pin 3） | 输入 | 闹钟中断，开漏输出 + 4.7kΩ 上拉到 3.3V |
| VCC | — | VCC（Pin 2） | 电源 | 3.3V 主电源（主电断电时自动切 CR2032） |
| VBAT | — | VBAT（Pin 14） | 电源 | CR2032 纽扣电池 3.0V |
| GND | — | GND（Pin 13） | 地 | 公共地 |

> **I2C 总线共享说明**：OV2640 SCCB（地址 0x30）与 DS3231（地址 0x68）地址不冲突，可共享 I2C0 总线（GPIO 4/5），节省 2 个 GPIO。两者均支持 100kHz，固件使用同一 I2C 实例即可。若需独立总线，可将 DS3231 移至 GPIO 47/48（I2C1），但需固件适配。

### 3.4 UART 调试接口

| 信号 | ESP32-S3 引脚 | 方向 | 说明 |
|------|--------------|------|------|
| TX | GPIO 43 | 输出 | U0TXD，115200bps，8N1 |
| RX | GPIO 44 | 输入 | U0RXD，连接 USB-TTL 适配器 |

> 调试串口仅用于开发与现场排障，量产可不焊接排针。USB-OTG（GPIO 19/20）作为备用烧录通道保留。

### 3.5 ADC 电池电压采样

| 信号 | ESP32-S3 引脚 | 方向 | 说明 |
|------|--------------|------|------|
| VBAT_SENSE | GPIO 1（ADC1_CH0） | 输入 | 模拟输入，电池电压经 100k/100k 分压 |

> 采样公式：`Vbat = ADC_value / 4095 × 3.3V × 2`（12-bit ADC，分压比 1:2）。详见 power_design.md。

### 3.6 GPIO 输出（MOSFET 受控上电）

| 信号 | ESP32-S3 引脚 | 方向 | 默认 | 控制对象 | 说明 |
|------|--------------|------|------|----------|------|
| CAM_EN | GPIO 2 | 输出 | HIGH（断电） | AO3401 Q1 栅极 | LOW=上电摄像头，10kΩ 上拉到 3.3V 确保复位时关断 |
| SD_EN | GPIO 42 | 输出 | HIGH（断电） | AO3401 Q2 栅极 | LOW=上电 SD 卡，10kΩ 上拉到 3.3V 确保复位时关断 |

> **默认断电策略**：GPIO 复位期间为高阻态，10kΩ 上拉确保 MOSFET 默认关断，避免外设在启动瞬间意外通电。

### 3.7 GPIO 输入（唤醒源）

| 信号 | ESP32-S3 引脚 | 方向 | 默认 | 唤醒方式 | 说明 |
|------|--------------|------|------|----------|------|
| RTC_INT | GPIO 21 | 输入 | HIGH（上拉） | EXT0，下降沿 | DS3231 闹钟中断，主唤醒源 |
| CONFIG_BTN | GPIO 0 | 输入 | HIGH（上拉） | EXT1，低电平 | 配置按钮，长按 5s 进 AP 模式 |

> **EXT0 vs EXT1**：EXT0 支持单个 RTC GPIO 的电平/边沿唤醒，功耗略高（约 +10µA）；EXT1 支持多个 RTC GPIO 组合唤醒，功耗更低。本项目 RTC_INT 用 EXT0（实时性高），CONFIG_BTN 用 EXT1（与未来扩展按键共用）。

---

## 4. 引脚使用统计

### 4.1 已分配引脚数

| 接口 | 引脚数 | 引脚列表 |
|------|--------|----------|
| 摄像头 DVP（含 SCCB + RESET） | 15 | 4,5,6,7,8,9,10,11,12,13,14,15,16,17,18 |
| SD 卡 SPI | 4 | 38,39,40,41 |
| UART 调试 | 2 | 43,44 |
| ADC 电池采样 | 1 | 1 |
| MOSFET 控制 | 2 | 2,42 |
| 唤醒输入 | 2 | 0,21 |
| **合计已分配** | **26** | — |

### 4.2 未分配引脚

| 引脚 | 原因 | 建议 |
|------|------|------|
| GPIO 3 | Strapping（JTAG 路由） | 留空，避免影响 JTAG |
| GPIO 19 | USB_D- | 保留原生 USB 烧录 |
| GPIO 20 | USB_D+ | 保留原生 USB 烧录 |
| GPIO 45 | Strapping（VDD_SPI 电压） | 留空，启动时必须 HIGH |
| GPIO 46 | Strapping（ROM 日志） | 留空 |
| GPIO 47 | 自由 | 预留扩展（I2C1 SDA / LoRa / 温湿度传感器） |
| GPIO 48 | 自由 | 预留扩展（I2C1 SCL / LoRa / 温湿度传感器） |

### 4.3 资源占用率

- 可用 GPIO：33 个
- 已分配：26 个（78.8%）
- 受限不可用：4 个 strapping + 2 个 USB = 6 个
- 自由预留：2 个（GPIO 47/48）

---

## 5. 引脚冲突检查清单

| 检查项 | 结果 |
|--------|------|
| 摄像头 DVP 数据线是否避开 strapping 引脚（0/3/45/46）？ | ✅ 使用 4~18，无冲突 |
| SD 卡 SPI 是否避开 strapping？ | ✅ 使用 38~42 |
| ADC 采样是否使用 ADC1（避开 Wi-Fi 冲突）？ | ✅ GPIO 1 = ADC1_CH0 |
| 唤醒源是否为 RTC GPIO（0~21）？ | ✅ GPIO 0、GPIO 21 |
| USB 引脚（19/20）是否保留？ | ✅ 未分配 |
| Strapping 引脚是否都有明确处理？ | ✅ GPIO 0 上拉（按钮），其余留空 |
| SPI Flash/PSRAM 引脚（26~37）是否未触碰？ | ✅ 全部避开 |
| MOSFET 控制脚是否默认关断（上拉）？ | ✅ 10kΩ 上拉到 3.3V |
| I2C 总线是否共享合理？ | ✅ SCCB + DS3231 共享 I2C0，地址不冲突 |

---

## 6. 固件 Pin 配置参考（ESP-IDF / Arduino）

以下配置可直接用于 `esp32-camera` 库与项目固件：

```c
/* 摄像头 DVP 引脚配置（esp32-camera camera_pin_t 结构） */
#define CAM_PIN_PWDN    -1   // 不使用，电源由 MOSFET 控制
#define CAM_PIN_RESET   14   // GPIO 14
#define CAM_PIN_XCLK    15   // GPIO 15
#define CAM_PIN_SIOD    4    // GPIO 4, SCCB SDA (与 DS3231 共享 I2C0)
#define CAM_PIN_SIOC    5    // GPIO 5, SCCB SCL
#define CAM_PIN_D7      16   // GPIO 16
#define CAM_PIN_D6      17   // GPIO 17
#define CAM_PIN_D5      18   // GPIO 18
#define CAM_PIN_D4      12   // GPIO 12
#define CAM_PIN_D3      10   // GPIO 10
#define CAM_PIN_D2      8    // GPIO 8
#define CAM_PIN_D1      9    // GPIO 9
#define CAM_PIN_D0      11   // GPIO 11
#define CAM_PIN_VSYNC   6    // GPIO 6
#define CAM_PIN_HREF    7    // GPIO 7
#define CAM_PIN_PCLK    13   // GPIO 13

/* SD 卡 SPI 引脚 */
#define SD_PIN_MOSI     38
#define SD_PIN_MISO     39
#define SD_PIN_SCK      40
#define SD_PIN_CS       41

/* I2C（DS3231，与摄像头 SCCB 共享） */
#define I2C_PIN_SDA     4    // 共享
#define I2C_PIN_SCL     5    // 共享
#define DS3231_ADDR     0x68

/* MOSFET 电源控制 */
#define PIN_CAM_EN      2    // LOW=上电, HIGH=断电
#define PIN_SD_EN       42   // LOW=上电, HIGH=断电

/* 唤醒源 */
#define PIN_RTC_INT     21   // EXT0 唤醒, 下降沿
#define PIN_CONFIG_BTN  0    // EXT1 唤醒, 低电平

/* ADC */
#define ADC_BATT_CH     ADC1_CHANNEL_0  // GPIO 1

/* UART 调试（默认 UART0，无需配置） */
// TX=GPIO43, RX=GPIO44
```

### 深度睡眠唤醒配置参考

```c
/* EXT0: DS3231 INT 唤醒（GPIO 21, 下降沿） */
esp_sleep_enable_ext0_wakeup(GPIO_NUM_21, 0);  // 0 = 低电平唤醒

/* EXT1: 配置按钮唤醒（GPIO 0, 低电平） */
esp_sleep_enable_ext1_wakeup(BIT64(GPIO_NUM_0), ESP_EXT1_WAKEUP_ALL_LOW);

/* 进入深度睡眠 */
esp_deep_sleep_start();
```

---

## 7. PCB 布线建议

1. **DVP 数据线等长**：GPIO 8~13、16~18 走线长度尽量一致，减少建立/保持时间偏差（PCLK 80MHz 时尤其重要）。
2. **XCLK 远离模拟信号**：GPIO 15 的 16MHz 方波易干扰 ADC，走线远离 GPIO 1（ADC1_CH0）。
3. **ADC 采样线短且加滤波**：GPIO 1 的分压网络靠近 ESP32 引脚，并联 100nF 滤波电容。
4. **I2C 上拉就近放置**：GPIO 4/5 的 4.7kΩ 上拉电阻靠近 ESP32 引脚，总线长度 < 50mm。
5. **MOSFET 栅极走线短**：GPIO 2、42 到 AO3401 栅极走线 < 20mm，栅极串联 100Ω 抑制振铃。
6. **电源去耦**：每个 IC 电源引脚就近放置 100nF + 10µF 去耦电容。
7. **地平面完整**：双层板底层尽量完整铺地，DVP 接口下方避免分割地平面。
