# wifi_upload 组件构建脚本（WiFi STA + HTTP 客户端上传照片）
idf_component_register(SRCS "wifi_upload.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi
                             esp_http_client
                             esp_event
                             esp_netif
                             nvs_flash
                             sd_storage
                             logger)
