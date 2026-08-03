#include "camera_hal.h"

#include <stdbool.h>
#include <string.h>

#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "camera_hal";

/* ============================================================ */
/*    摄像头 DVP / SCCB 引脚配置（严格遵循 gpio_allocation.md）  */
/* ============================================================ */

/* OV2640 并行数据线 D0~D7（Y2~Y9），按 ESP32-S3 LCD_CAM 引脚分配 */
#define CAM_PIN_D0      11   /* Y2  */
#define CAM_PIN_D1       9   /* Y3  */
#define CAM_PIN_D2       8   /* Y4  */
#define CAM_PIN_D3      10   /* Y5  */
#define CAM_PIN_D4      12   /* Y6  */
#define CAM_PIN_D5      18   /* Y7  */
#define CAM_PIN_D6      17   /* Y8  */
#define CAM_PIN_D7      16   /* Y9  */
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4   /* SCCB SDA，与 DS3231 共享 I2C0 */
#define CAM_PIN_SIOC     5   /* SCCB SCL，与 DS3231 共享 I2C0 */
#define CAM_PIN_RESET   14   /* 低电平复位，常态 HIGH */
#define CAM_PIN_PWDN    -1   /* 不使用：PWDN 直接接地，电源由 MOSFET 控制 */

/* XCLK 主时钟：OV2640 推荐 16MHz（gpio_allocation.md 约定） */
#define CAM_XCLK_FREQ_HZ 16000000

/* 当前未归还的帧缓冲指针，release_fb / deinit 用以归还 */
static camera_fb_t *s_held_fb = NULL;
/* 标记相机是否已初始化，避免重复 init / 防止 deinit 误调用 */
static bool s_inited = false;

/* ============================================================ */
/*                  分辨率字符串 -> framesize 映射              */
/* ============================================================ */

static framesize_t resolution_to_framesize(const char *resolution,
                                           framesize_t fallback)
{
    if (resolution == NULL) {
        return fallback;
    }
    /* 同时支持名称（UXGA/SXGA/VGA）与 "宽x高" 两种写法 */
    if (strcmp(resolution, "UXGA") == 0 || strcmp(resolution, "1600x1200") == 0) {
        return FRAMESIZE_UXGA;
    }
    if (strcmp(resolution, "SXGA") == 0 || strcmp(resolution, "1280x1024") == 0) {
        return FRAMESIZE_SXGA;
    }
    if (strcmp(resolution, "XGA") == 0 || strcmp(resolution, "1024x768") == 0) {
        return FRAMESIZE_XGA;
    }
    if (strcmp(resolution, "SVGA") == 0 || strcmp(resolution, "800x600") == 0) {
        return FRAMESIZE_SVGA;
    }
    if (strcmp(resolution, "VGA") == 0 || strcmp(resolution, "640x480") == 0) {
        return FRAMESIZE_VGA;
    }
    ESP_LOGW(TAG, "未知分辨率 \"%s\"，使用回退值", resolution);
    return fallback;
}

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t camera_hal_init(void)
{
    if (s_inited) {
        ESP_LOGW(TAG, "摄像头已初始化，跳过重复 init");
        return ESP_OK;
    }

    camera_config_t config = {
        /* DVP 并行数据线 */
        .pin_d0 = CAM_PIN_D0,
        .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2,
        .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4,
        .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6,
        .pin_d7 = CAM_PIN_D7,
        /* 同步与时钟 */
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,
        .pin_xclk  = CAM_PIN_XCLK,
        .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
        /* SCCB（I2C），与 DS3231 共享 I2C0 总线 */
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .sccb_i2c_port = I2C_NUM_0,
        /* PWDN 不使用；RESET 低电平复位 */
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        /* LEDC 用作 XCLK 时钟源（ESP32-S3 由 LCD_CAM 提供，仍需配置） */
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer   = LEDC_TIMER_0,
        /* 像素格式：JPEG；帧缓冲放 PSRAM */
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_UXGA,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        /* JPEG 质量 4~63，越小越好；默认 12 平衡体积与画质 */
        .jpeg_quality = 12,
        /* 双帧缓冲 + 抓取最新帧，降低拍照延迟 */
        .fb_count = 2,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_inited = true;
    s_held_fb = NULL;
    ESP_LOGI(TAG, "OV2640 摄像头初始化成功（UXGA, JPEG, PSRAM 帧缓冲）");
    return ESP_OK;
}

esp_err_t camera_hal_capture(uint8_t **out_buf, size_t *out_len,
                             const char *resolution, int quality)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    if (!s_inited) {
        ESP_LOGE(TAG, "摄像头未初始化，无法拍照");
        return ESP_ERR_INVALID_STATE;
    }
    /* 同一时刻只允许持有一帧未归还，防止帧缓冲泄漏 */
    if (s_held_fb != NULL) {
        ESP_LOGW(TAG, "存在未释放的帧缓冲，先归还再拍照");
        esp_camera_fb_return(s_held_fb);
        s_held_fb = NULL;
    }

    /* 应用分辨率与质量参数 */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        framesize_t fs = resolution_to_framesize(resolution, FRAMESIZE_UXGA);
        if (sensor->set_framesize(sensor, fs) != 0) {
            ESP_LOGW(TAG, "set_framesize 失败，沿用当前分辨率");
        }
        /* JPEG 质量 clamp 到 [4, 63] */
        int q = quality;
        if (q < 4) {
            q = 4;
        } else if (q > 63) {
            q = 63;
        }
        if (sensor->set_quality(sensor, q) != 0) {
            ESP_LOGW(TAG, "set_quality(%d) 失败，沿用当前质量", q);
        }
    } else {
        ESP_LOGW(TAG, "esp_camera_sensor_get 返回 NULL，跳过参数设置");
    }

    /* 抓取一帧 JPEG */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "esp_camera_fb_get 失败（帧缓冲耗尽或超时）");
        return ESP_FAIL;
    }
    if (fb->len == 0 || fb->buf == NULL) {
        ESP_LOGE(TAG, "抓取到空帧（len=%u）", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    s_held_fb = fb;
    *out_buf = fb->buf;
    *out_len = fb->len;

    ESP_LOGI(TAG, "拍照成功：%s, 质量=%d, %u 字节",
             resolution ? resolution : "default", quality, (unsigned)fb->len);
    return ESP_OK;
}

esp_err_t camera_hal_release_fb(void)
{
    if (s_held_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_camera_fb_return(s_held_fb);
    s_held_fb = NULL;
    return ESP_OK;
}

esp_err_t camera_hal_deinit(void)
{
    /* 归还任何未释放的帧缓冲，避免泄漏 */
    if (s_held_fb != NULL) {
        esp_camera_fb_return(s_held_fb);
        s_held_fb = NULL;
    }

    if (!s_inited) {
        return ESP_OK;
    }

    esp_err_t ret = esp_camera_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_deinit 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_inited = false;
    ESP_LOGI(TAG, "摄像头已反初始化（外设电源由 power_mgmt 控制）");
    return ESP_OK;
}
