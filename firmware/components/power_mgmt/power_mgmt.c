#include "power_mgmt.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "power_mgmt";

/* ============================================================ */
/*      引脚定义（严格遵循 gpio_allocation.md / power_design.md）*/
/* ============================================================ */

/* MOSFET 受控电源：P-MOSFET，HIGH=断电（Vgs=0 关断），LOW=上电（Vgs=-3.3V 导通） */
#define PIN_CAM_EN      GPIO_NUM_2    /* AO3401 Q1 栅极，控制摄像头电源 */
#define PIN_SD_EN       GPIO_NUM_42   /* AO3401 Q2 栅极，控制 SD 卡电源 */

/* DS3231 闹钟中断（主唤醒源，EXT0 下降沿/低电平） */
#define PIN_RTC_INT     GPIO_NUM_21

/* 配置按钮（strapping 引脚，需上拉；长按 5s 进 AP 模式） */
#define PIN_CONFIG_BTN  GPIO_NUM_0

/* ADC 电池电压采样：GPIO1 = ADC1_CH0，分压 100k/100k → V_sense = Vbat/2 */
#define BATT_ADC_UNIT       ADC_UNIT_1
#define BATT_ADC_CHANNEL    ADC1_CHANNEL_0
#define BATT_ADC_ATTEN      ADC_ATTEN_DB_11   /* 满量程 ~3.1V，覆盖 V_sense 最大 1.825V */
#define BATT_ADC_BITWIDTH   ADC_BITWIDTH_12

/* ============================================================ */
/*      LiFePO4 电池参数（参考 power_design.md 放电曲线）         */
/* ============================================================ */

#define BATT_V_FULL      3.4f    /* 视为 100% 电量的电压阈值 */
#define BATT_V_MID       3.2f    /* 中段拐点：100%↔50% 与 50%↔10% 的分界 */
#define BATT_V_LOW       3.0f    /* 低电告警/省电策略触发阈值 */
#define BATT_V_EMPTY     3.0f    /* 视为 0% 的电压（与 V_LOW 一致） */

/* ADC 分压比：R1=R2=100kΩ，Vbat = V_sense × 2 */
#define BATT_DIVIDER_RATIO  2.0f

/* 多次采样次数（均值滤波） */
#define BATT_SAMPLE_COUNT   32

/* ADC 满量程电压（11dB 衰减，无校准时回退用） */
#define ADC_FULLSCALE_MV    3100.0f
#define ADC_MAX_RAW         4095

/* ============================================================ */
/*                       模块内部状态                            */
/* ============================================================ */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_adc_cali   = NULL;
static bool s_adc_inited = false;

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t power_mgmt_init(void)
{
    /* ---- 1. MOSFET 电源控制 GPIO 配置为输出，默认 HIGH（断电） ---- */
    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(PIN_CAM_EN) | BIT64(PIN_SD_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&out_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MOSFET GPIO 配置失败: %s", esp_err_to_name(ret));
        return ret;
    }
    /* P-MOSFET 高电平关断：复位后立即确保外设断电（10kΩ 上拉亦保证默认关断） */
    gpio_set_level(PIN_CAM_EN, 1);
    gpio_set_level(PIN_SD_EN, 1);

    /* ---- 2. 配置按钮 GPIO0 输入带上拉（strapping 引脚，避免启动误触发） ---- */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = BIT64(PIN_CONFIG_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);   /* 失败不影响主流程，仅记录 */

    /* ---- 3. ADC1_CH0 电池电压采样（adc_oneshot 新 API） ---- */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATT_ADC_ATTEN,
        .bitwidth = BATT_ADC_BITWIDTH,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, BATT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- 4. ADC 校准（line fitting，依赖 eFuse 校准数据；无则回退公式） ---- */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BATT_ADC_UNIT,
        .atten = BATT_ADC_ATTEN,
        .bitwidth = BATT_ADC_BITWIDTH,
    };
    ret = adc_cali_create_line_fitting(&cali_cfg, &s_adc_cali);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC 校准初始化失败（%s），将使用 raw/4095×3.1 公式回退",
                 esp_err_to_name(ret));
        s_adc_cali = NULL;   /* 回退模式：无校准 */
    }

    s_adc_inited = true;
    ESP_LOGI(TAG, "电源管理初始化成功（CAM_EN=GPIO%d, SD_EN=GPIO%d, ADC1_CH0 已校准=%s）",
             (int)PIN_CAM_EN, (int)PIN_SD_EN,
             s_adc_cali ? "yes" : "no");
    return ESP_OK;
}

esp_err_t power_enable_peripheral(peripheral_t p)
{
    if (p < 0 || p >= P_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_num_t pin;
    switch (p) {
    case P_CAM:
        pin = PIN_CAM_EN;
        break;
    case P_SD:
        pin = PIN_SD_EN;
        break;
    default:
        /* P_SENSOR 等未连接 MOSFET，无硬件对应 */
        ESP_LOGW(TAG, "外设 %d 无对应 MOSFET，跳过上电", (int)p);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* P-MOSFET：拉低栅极 → Vgs=-3.3V → 导通 → 外设上电 */
    gpio_set_level(pin, 0);
    /* 上电稳定延时，等待电源与外设就绪（摄像头建议 200ms 预热，此处取 100ms 通用值） */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "外设 %d 已上电（GPIO%d=LOW）", (int)p, (int)pin);
    return ESP_OK;
}

esp_err_t power_disable_peripheral(peripheral_t p)
{
    if (p < 0 || p >= P_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_num_t pin;
    switch (p) {
    case P_CAM:
        pin = PIN_CAM_EN;
        break;
    case P_SD:
        pin = PIN_SD_EN;
        break;
    default:
        ESP_LOGW(TAG, "外设 %d 无对应 MOSFET，跳过断电", (int)p);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* P-MOSFET：拉高栅极 → Vgs=0 → 关断 → 外设断电 */
    gpio_set_level(pin, 1);
    ESP_LOGI(TAG, "外设 %d 已断电（GPIO%d=HIGH）", (int)p, (int)pin);
    return ESP_OK;
}

esp_err_t power_read_battery_voltage(float *voltage)
{
    if (voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *voltage = 0.0f;

    if (!s_adc_inited) {
        ESP_LOGE(TAG, "ADC 未初始化，无法采样电池电压");
        return ESP_ERR_INVALID_STATE;
    }

    /* 多次采样取平均，降低 ADC 噪声（分压网络并联 100nF 已做硬件滤波） */
    int raw_sum = 0;
    int valid = 0;
    for (int i = 0; i < BATT_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, BATT_ADC_CHANNEL, &raw) == ESP_OK) {
            raw_sum += raw;
            valid++;
        }
    }
    if (valid == 0) {
        ESP_LOGE(TAG, "ADC 采样全部失败");
        return ESP_FAIL;
    }
    int raw_avg = raw_sum / valid;

    /* 将 ADC 原始值转换为 ADC 引脚电压（mV）：
     *   - 有校准：adc_cali_raw_to_voltage 利用 eFuse 校准数据
     *   - 无校准：回退公式 V_sense = raw/4095 × 3.1V */
    int v_sense_mv = 0;
    if (s_adc_cali != NULL) {
        if (adc_cali_raw_to_voltage(s_adc_cali, raw_avg, &v_sense_mv) != ESP_OK) {
            /* 校准转换失败则回退公式 */
            v_sense_mv = (int)((float)raw_avg / ADC_MAX_RAW * ADC_FULLSCALE_MV);
        }
    } else {
        v_sense_mv = (int)((float)raw_avg / ADC_MAX_RAW * ADC_FULLSCALE_MV);
    }

    /* 分压 100k/100k：Vbat = V_sense × 2 */
    float vbat = (v_sense_mv / 1000.0f) * BATT_DIVIDER_RATIO;
    *voltage = vbat;

    return ESP_OK;
}

float power_get_battery_percent(float voltage)
{
    /* LiFePO4 放电曲线分段映射（见 power_design.md 第 7.5 节策略） */
    if (voltage >= BATT_V_FULL) {
        return 100.0f;
    }
    if (voltage <= BATT_V_EMPTY) {
        return 0.0f;
    }
    if (voltage > BATT_V_MID) {
        /* 3.4V→100%, 3.2V→50% */
        return 50.0f + (voltage - BATT_V_MID) / (BATT_V_FULL - BATT_V_MID) * 50.0f;
    }
    /* 3.2V→50%, 3.0V→10% */
    return 10.0f + (voltage - BATT_V_LOW) / (BATT_V_MID - BATT_V_LOW) * 40.0f;
}

esp_err_t power_enter_deep_sleep(uint32_t seconds, gpio_num_t wake_gpio)
{
    ESP_LOGI(TAG, "准备进入深度睡眠（兜底 timer=%lu s）", (unsigned long)seconds);

    /* 关闭 WiFi/BT 无线电以降低睡眠功耗。
     * esp_deep_sleep_start() 会自动关闭大部分 RTC 外设与射频，但若 WiFi
     * 此前已连接，调用方（wifi_upload）应已 graceful disconnect。
     * 这里不直接调用 esp_wifi_stop() 以避免引入 esp_wifi 依赖耦合；
     * 射频在深睡期间会被硬件自动断电。 */

    /* 主唤醒源：DS3231 闹钟中断（GPIO21，低电平触发）。
     * EXT0 支持单个 RTC GPIO 的电平唤醒，适合实时性要求高的 RTC INT。
     * 注意：DS3231 INT 为开漏输出 + 上拉，闹钟触发时拉低，故 level=0。 */
    esp_err_t ret = esp_sleep_enable_ext0_wakeup(PIN_RTC_INT, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "EXT0 唤醒配置失败(GPIO%d): %s",
                 (int)PIN_RTC_INT, esp_err_to_name(ret));
        /* 继续尝试其它唤醒源，避免无法睡眠 */
    }

    /* 附加唤醒源：如配置按钮 GPIO0（EXT1 低电平唤醒）。
     * wake_gpio==0 表示不启用附加引脚（与 .h 注释一致）。 */
    if (wake_gpio != 0 && wake_gpio != PIN_RTC_INT) {
        ret = esp_sleep_enable_ext1_wakeup(BIT64(wake_gpio),
                                           ESP_EXT1_WAKEUP_ALL_LOW);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "EXT1 唤醒配置失败(GPIO%d): %s",
                     (int)wake_gpio, esp_err_to_name(ret));
        }
    }

    /* 兜底唤醒源：定时器。防止 DS3231 闹钟异常（如电池耗尽）导致永久睡眠。
     * seconds=0 时不启用 timer，完全依赖 RTC INT。 */
    if (seconds > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    }

    /* 进入深度睡眠。唤醒后系统复位，重新执行 app_main()，
     * 因此本函数正常情况下不会返回。 */
    esp_deep_sleep_start();

    /* 仅在 deep_sleep_start 异常失败时才会执行到此处 */
    ESP_LOGE(TAG, "esp_deep_sleep_start 异常返回！");
    return ESP_FAIL;
}

bool power_is_battery_low(float voltage)
{
    /* 阈值 3.0V：低于则进入省电保护策略 */
    return voltage < BATT_V_LOW;
}

bool power_should_skip_capture(float voltage, uint8_t *interval_minutes)
{
    if (voltage < BATT_V_LOW) {
        /* 低于 3.0V：停止拍照，仅维持 RTC + 深睡，等待太阳能回充 */
        ESP_LOGW(TAG, "电池 %.2fV 低于 %.1fV，跳过本次拍照", voltage, BATT_V_LOW);
        return true;
    }

    if (voltage < BATT_V_MID) {
        /* 3.0~3.2V：拍照间隔翻倍，降低功耗延长运行 */
        if (interval_minutes != NULL && *interval_minutes > 0) {
            uint16_t doubled = (uint16_t)(*interval_minutes) * 2;
            /* 防溢出（uint8_t 上限 255） */
            *interval_minutes = (doubled > 255) ? 255 : (uint8_t)doubled;
            ESP_LOGW(TAG, "电池 %.2fV 处于低电区间，间隔翻倍为 %u 分钟",
                     voltage, (unsigned)*interval_minutes);
        }
        return false;
    }

    /* >= 3.2V：维持原间隔，正常拍照 */
    return false;
}
