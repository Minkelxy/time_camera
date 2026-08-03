# app 组件构建脚本（应用主业务循环）
# 依赖全部业务子组件，串联拍照/存储/回传/睡眠流程
idf_component_register(SRCS "app.c"
                    INCLUDE_DIRS "include"
                    REQUIRES camera_hal
                             sd_storage
                             power_mgmt
                             config_mgr
                             wifi_upload
                             logger
                             ds3231
                             config_portal
                             esp_event)
