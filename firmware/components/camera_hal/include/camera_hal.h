#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化摄像头 HAL
 *
 * 内部调用 esp_camera 组件完成 DVP/SCCB 引脚与 PSRAM 帧缓冲配置。
 * 电源使能由 power_mgmt 控制，本函数假定外设已上电。
 *
 * @return ESP_OK 成功
 */
esp_err_t camera_hal_init(void);

/**
 * @brief 拍照并获取 JPEG 数据
 *
 * 调用 esp_camera_fb_get() 获取一帧 JPEG。返回的缓冲区由 esp_camera
 * 内部管理，调用方使用完毕后必须调用 camera_hal_release_fb() 归还，
 * 否则帧缓冲会被占满导致后续拍照失败。
 *
 * @param out_buf    输出 JPEG 数据缓冲区指针（由内部管理，调用方无需 free）
 * @param out_len    输出 JPEG 数据长度
 * @param resolution 分辨率字符串，支持 "UXGA"/"SXGA"/"VGA" 或
 *                   "1600x1200"/"1280x1024"/"800x600"/"640x480"
 * @param quality    JPEG 质量（数值越小质量越高，esp_camera 范围 4~63）
 * @return ESP_OK 成功
 */
esp_err_t camera_hal_capture(uint8_t **out_buf, size_t *out_len,
                             const char *resolution, int quality);

/**
 * @brief 释放 camera_hal_capture() 返回的帧缓冲
 *
 * 必须在数据处理完成后调用，以归还帧缓冲给 esp_camera 复用。
 * 若已调用 camera_hal_deinit() 则无需再调用本函数。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 当前无未释放的帧
 */
esp_err_t camera_hal_release_fb(void);

/**
 * @brief 反初始化摄像头，释放资源
 *
 * 内部归还任何未释放的帧缓冲并调用 esp_camera_deinit()。
 * 注意：仅释放相机驱动资源，外设电源仍由 power_mgmt 控制。
 *
 * @return ESP_OK 成功
 */
esp_err_t camera_hal_deinit(void);

#ifdef __cplusplus
}
#endif
