# main 组件构建脚本
# 入口 app_main()，依赖 app / power_mgmt / config_mgr / logger / sd_storage / config_portal
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app
                             power_mgmt
                             config_mgr
                             logger
                             sd_storage
                             config_portal
                             driver
                             esp_hw_support)
