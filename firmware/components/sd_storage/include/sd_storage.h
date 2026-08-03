#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SD 卡并挂载 FATFS 到 /sdcard
 *
 * 使用 SDMMC 1-bit / 4-bit 模式（具体由 BSP 决定）。
 *
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_init(void);

/**
 * @brief 写入文件（覆盖）
 *
 * @param path 文件路径（含 /sdcard 前缀）
 * @param data 数据指针
 * @param len  数据长度
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_write_file(const char *path, const uint8_t *data, size_t len);

/**
 * @brief 读取文件
 *
 * @param path    文件路径
 * @param out_buf 输出缓冲区指针（调用方负责 free）
 * @param out_len 输出数据长度
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_read_file(const char *path, uint8_t **out_buf, size_t *out_len);

/**
 * @brief 创建目录（递归）
 *
 * @param path 目录路径
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_mkdir(const char *path);

/**
 * @brief 列出目录下的文件/子目录名
 *
 * @param path      目录路径
 * @param out_list  输出字符串数组（调用方负责逐项 free 与数组 free）
 * @param out_count 输出条目数量
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_list_dir(const char *path, char ***out_list, size_t *out_count);

/**
 * @brief 获取存储空间
 *
 * @param free_bytes  输出可用字节数
 * @param total_bytes 输出总字节数
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes);

/**
 * @brief 删除文件或目录
 *
 * @param path 路径
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_delete_path(const char *path);

/**
 * @brief 卸载 FATFS 并释放 SD 卡资源
 *
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_deinit(void);

#ifdef __cplusplus
}
#endif
