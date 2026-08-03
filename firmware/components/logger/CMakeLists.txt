# logger 组件构建脚本（SD 卡日志 + ESP_LOG 双通道）
idf_component_register(SRCS "logger.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fatfs
                             vfs
                             sd_storage
                             freertos)
