# config_mgr 组件构建脚本（cJSON 解析 /sdcard/config.json）
idf_component_register(SRCS "config_mgr.c"
                    INCLUDE_DIRS "include"
                    REQUIRES json
                             fatfs
                             vfs
                             sd_storage)
