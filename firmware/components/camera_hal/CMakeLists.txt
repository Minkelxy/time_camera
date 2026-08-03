# camera_hal 组件构建脚本
#
# 摄像头 HAL 封装，依赖 espressif/esp_camera 托管组件（见 idf_component.yml）。
# esp_camera 通过 SDSPI/LCD_CAM DVP 接口驱动 OV2640，并使用 PSRAM 帧缓冲。
idf_component_register(SRCS "camera_hal.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver
                             esp_camera)
