# sd_storage 组件构建脚本（SD 卡 + FATFS + VFS）
idf_component_register(SRCS "sd_storage.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fatfs
                             vfs
                             sdmmc
                             driver)
