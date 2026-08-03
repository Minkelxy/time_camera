# config_portal 组件构建脚本（SoftAP + HTTP Server Web 配置门户）
idf_component_register(SRCS "config_portal.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi
                             esp_http_server
                             esp_event
                             esp_netif
                             nvs_flash
                             config_mgr
                             sd_storage
                             power_mgmt
                             logger)
