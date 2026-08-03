# power_mgmt 组件构建脚本（电源管理 / ADC / 深度睡眠）
idf_component_register(SRCS "power_mgmt.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver
                             esp_adc
                             esp_hw_support)
