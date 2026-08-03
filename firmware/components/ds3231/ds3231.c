#include "ds3231.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ds3231";

/* I2C 端口与引脚配置（init 时保存），供后续读写复用 */
static i2c_port_t s_port = I2C_NUM_0;
/* 标记 I2C 驱动是否已由本组件安装（用于避免重复 install） */
static bool s_i2c_installed = false;

/* DS3231 I2C 通信超时（ticks），RTC 响应很快但留足裕量 */
#define DS3231_I2C_TIMEOUT_MS 100

/* ============================================================ */
/*                       BCD <-> BIN 转换工具                    */
/* ============================================================ */

/* 二进制 -> BCD（范围 0~99） */
static inline uint8_t bin2bcd(uint8_t v)
{
    return ((v / 10) << 4) | (v % 10);
}

/* BCD -> 二进制 */
static inline uint8_t bcd2bin(uint8_t b)
{
    return ((b >> 4) * 10) + (b & 0x0F);
}

/* ============================================================ */
/*                       I2C 原子读写操作                        */
/* ============================================================ */

/* 向指定寄存器写入若干字节（首字节为寄存器地址，后接数据） */
static esp_err_t ds3231_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    /* 组装 [reg, data...] 缓冲区，一次 I2C 写事务完成 */
    uint8_t buf[1 + 8] = {0};   /* DS3231 单次最多写 7 个时间寄存器 + 1 地址 */
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    if (len > 0 && data != NULL) {
        memcpy(&buf[1], data, len);
    }
    return i2c_master_write_to_device(s_port, DS3231_I2C_ADDR, buf, len + 1,
                                      pdMS_TO_TICKS(DS3231_I2C_TIMEOUT_MS));
}

/* 从指定寄存器读取若干字节：先写寄存器地址，再 restart 读 */
static esp_err_t ds3231_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(s_port, DS3231_I2C_ADDR,
                                        &reg, 1, data, len,
                                        pdMS_TO_TICKS(DS3231_I2C_TIMEOUT_MS));
}

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t ds3231_init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl)
{
    if (sda < 0 || scl < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_port = port;

    /* 仅在尚未安装时配置 I2C 主机。
     * 注意：本 I2C0 总线与 OV2640 SCCB 共享 GPIO4/GPIO5（见 gpio_allocation.md），
     * 若 esp_camera 也尝试在同端口安装驱动会冲突；esp_camera 通过
     * camera_config_t.sccb_i2c_port = I2C_NUM_0 复用本总线即可。 */
    if (!s_i2c_installed) {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 400000,   /* DS3231 支持 400kHz Fast Mode */
            .clk_flags = 0,
        };
        esp_err_t ret = i2c_param_config(s_port, &conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2c_param_config 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = i2c_driver_install(s_port, I2C_MODE_MASTER, 0, 0, 0);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "i2c_driver_install 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        s_i2c_installed = true;
    }

    /* 探测 DS3231：读取秒寄存器 0x00，应答正常即在线 */
    uint8_t sec = 0;
    esp_err_t ret = ds3231_read_reg(0x00, &sec, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 在线检测失败（地址 0x%02X）: %s",
                 DS3231_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    /* 清除可能的残留闹钟标志，避免 INT 引脚保持低电平 */
    ds3231_clear_alarm();

    /* 若振荡器曾停振（OSF=1），提示用户需设置时间 */
    uint8_t status = 0;
    ds3231_read_reg(0x0F, &status, 1);
    if (status & 0x80) {
        ESP_LOGW(TAG, "DS3231 振荡器停振标志 OSF=1，时间可能失效，请调用 ds3231_set_time()");
        /* 清除 OSF */
        status &= ~0x80;
        ds3231_write_reg(0x0F, &status, 1);
    }

    ESP_LOGI(TAG, "DS3231 初始化成功（I2C port=%d, SDA=%d, SCL=%d, 400kHz）",
             (int)s_port, (int)sda, (int)scl);
    return ESP_OK;
}

esp_err_t ds3231_get_time(struct tm *time)
{
    if (time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 0x00~0x06: 秒/分/时/星期/日/月/年（全 BCD） */
    uint8_t regs[7] = {0};
    esp_err_t ret = ds3231_read_reg(0x00, regs, sizeof(regs));
    if (ret != ESP_OK) {
        return ret;
    }

    memset(time, 0, sizeof(*time));

    /* 秒：bit6 为保留（0），低 6 位 BCD */
    time->tm_sec = bcd2bin(regs[0] & 0x7F);
    time->tm_min = bcd2bin(regs[1] & 0x7F);

    /* 时寄存器 bit6 = 12/24 模式选择位；本项目固定 24 小时制 */
    if (regs[2] & 0x40) {
        /* 12 小时模式：bit5=AM/PM(1=PM)，低 5 位 BCD 为 1~12 */
        uint8_t hour = bcd2bin(regs[2] & 0x1F);
        if (regs[2] & 0x20) {
            hour = (hour % 12) + 12;   /* PM: 12pm=12, 1pm=13 ... 11pm=23 */
        } else {
            hour = hour % 12;          /* AM: 12am=0, 1am=1 ... 11am=11 */
        }
        time->tm_hour = hour;
    } else {
        /* 24 小时模式：bit5 为 20 小时位，低 5 位 BCD 为 0~19 */
        time->tm_hour = bcd2bin(regs[2] & 0x3F);
    }

    /* 星期（1~7，DS3231 不强制约定起始日，本项目 Sunday=1） */
    time->tm_wday = (regs[3] & 0x07) - 1;   /* struct tm: 0=Sunday */
    time->tm_mday = bcd2bin(regs[4] & 0x3F);
    /* 月：bit7=世纪位（0=1900s, 1=2000s），低 5 位 BCD */
    time->tm_mon = bcd2bin(regs[5] & 0x1F) - 1;   /* struct tm: 0~11 */
    time->tm_year = bcd2bin(regs[6]) + 100;       /* struct tm: 年份 - 1900，DS3231 默认 2000+ */

    /* 自动推导年内天数（部分 mktime 实现需要） */
    time->tm_yday = 0;
    time->tm_isdst = -1;

    return ESP_OK;
}

esp_err_t ds3231_set_time(const struct tm *time)
{
    if (time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t regs[7] = {0};
    regs[0] = bin2bcd((uint8_t)time->tm_sec);
    regs[1] = bin2bcd((uint8_t)time->tm_min);
    /* 24 小时模式：bit6=0 */
    regs[2] = bin2bcd((uint8_t)time->tm_hour) & 0x3F;
    /* 星期：struct tm 0=Sunday -> DS3231 1~7 */
    regs[3] = (uint8_t)((time->tm_wday + 1) & 0x07);
    if (regs[3] == 0) {
        regs[3] = 1;
    }
    regs[4] = bin2bcd((uint8_t)time->tm_mday);
    /* 月：保持世纪位 0（2000s）；struct tm 0~11 -> DS3231 1~12 */
    regs[5] = bin2bcd((uint8_t)(time->tm_mon + 1)) & 0x1F;
    /* 年：struct tm = year-1900；DS3231 0~99 对应 2000~2099 */
    regs[6] = bin2bcd((uint8_t)(time->tm_year - 100));

    esp_err_t ret = ds3231_write_reg(0x00, regs, sizeof(regs));
    if (ret == ESP_OK) {
        /* 清除振荡器停振标志（写时间后视为已校时） */
        uint8_t status = 0;
        ds3231_read_reg(0x0F, &status, 1);
        status &= ~0x80;
        ds3231_write_reg(0x0F, &status, 1);
        ESP_LOGI(TAG, "时间已设置: %04d-%02d-%02d %02d:%02d:%02d",
                 time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
                 time->tm_hour, time->tm_min, time->tm_sec);
    }
    return ret;
}

esp_err_t ds3231_set_alarm(uint8_t minute_interval)
{
    if (minute_interval == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 读取当前时间，计算下次闹钟时刻 = now + minute_interval 分钟 */
    struct tm now = {0};
    esp_err_t ret = ds3231_get_time(&now);
    if (ret != ESP_OK) {
        return ret;
    }

    int total_min = now.tm_min + minute_interval;
    int alarm_hour = now.tm_hour + total_min / 60;
    int alarm_min = total_min % 60;
    alarm_hour = alarm_hour % 24;   /* 跨日自动取模 */

    /* 使用 Alarm2 "小时+分钟匹配" 模式：
     *   - 0x0B Alarm2 分钟：bit7=A2M2 (0=匹配), 低位 BCD
     *   - 0x0C Alarm2 小时：bit7=A2M3 (0=匹配), bit6=12/24, 低 5 位 BCD
     *   - 0x0D Alarm2 日期：bit7=A2M4 (1=忽略), bit6=DY/DT
     * 匹配 hour+minute 可正确处理任意正整数分钟间隔（含 60 的倍数），
     * 跨小时/跨日均可，不会在设置瞬间误触发。 */
    uint8_t alarm_regs[3] = {0};
    alarm_regs[0] = (bin2bcd((uint8_t)alarm_min) & 0x7F);              /* A2M2=0 匹配分钟 */
    alarm_regs[1] = (bin2bcd((uint8_t)alarm_hour) & 0x3F);             /* A2M3=0 匹配小时, 24h */
    alarm_regs[2] = 0x80;                                              /* A2M4=1 忽略日期, DY/DT=0 */
    ret = ds3231_write_reg(0x0B, alarm_regs, sizeof(alarm_regs));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 配置控制寄存器 0x0E：
     *   bit7 EOSC=0  振荡器使能
     *   bit6 BBSQW=0 电池供电时 INT/SQW 不输出方波
     *   bit5 CONV=0  不强制温度转换
     *   bit2 INTCN=1 INT/SQW 输出闹钟中断（而非方波）
     *   bit1 A2IE=1  使能 Alarm2 中断
     *   bit0 A1IE=0  禁用 Alarm1 中断
     * => 0x06 */
    uint8_t ctrl = 0x06;
    ret = ds3231_write_reg(0x0E, &ctrl, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Alarm2 已设置：下次唤醒 %02d:%02d（间隔 %u 分钟）",
             alarm_hour, alarm_min, minute_interval);
    return ESP_OK;
}

esp_err_t ds3231_clear_alarm(void)
{
    /* 状态寄存器 0x0F：bit1=A2F, bit0=A1F；写 0 清除标志 */
    uint8_t status = 0;
    esp_err_t ret = ds3231_read_reg(0x0F, &status, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    status &= ~0x03;   /* 清除 A1F / A2F */
    return ds3231_write_reg(0x0F, &status, 1);
}

esp_err_t ds3231_get_temperature(float *temp)
{
    if (temp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 温度寄存器 0x11（高字节，有符号补码）+ 0x12（低字节，bit7:6 = 0.25 分辨率） */
    uint8_t regs[2] = {0};
    esp_err_t ret = ds3231_read_reg(0x11, regs, sizeof(regs));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 高字节为 8 位有符号整数部分 */
    int8_t msb = (int8_t)regs[0];
    /* 低字节高 2 位为小数部分（00=0, 01=0.25, 10=0.5, 11=0.75） */
    float frac = (regs[1] >> 6) & 0x03;
    *temp = (float)msb + frac * 0.25f;

    return ESP_OK;
}
