# ds3231 组件构建脚本（DS3231 高精度 RTC 驱动，I2C 接口）
idf_component_register(SRCS "ds3231.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver)
