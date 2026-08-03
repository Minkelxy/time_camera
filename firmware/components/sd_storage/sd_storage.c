#include "sd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sd_storage";

#define SD_MOUNT_POINT "/sdcard"

/* SD 卡 SPI 引脚（严格遵循 gpio_allocation.md） */
#define SD_PIN_MOSI     38
#define SD_PIN_MISO     39
#define SD_PIN_SCK      40
#define SD_PIN_CS       41

/* 挂载后保留的 SD 卡句柄，供 deinit 使用 */
static sdmmc_card_t *s_card = NULL;
/* 标记 SPI 总线是否已由本组件初始化（用于 deinit 时正确释放） */
static bool s_spi_bus_inited = false;
/* 标记是否已挂载，避免重复 init */
static bool s_mounted = false;

/* ============================================================ */
/*                          辅助函数                             */
/* ============================================================ */

/* 递归创建目录（类似 mkdir -p）。
 * 路径形如 "/sdcard/photos/2024/01"，逐级创建已不存在的子目录。 */
static esp_err_t mkdir_p(const char *path)
{
    char tmp[256] = {0};
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(tmp, path, len);

    /* 去掉末尾可能的 '/'，便于逐级处理 */
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    /* 从首个非根 '/' 处开始逐级创建。
     * 跳过开头的 '/'（绝对路径首字符），从第二个组件开始。 */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir(%s) 失败: %s", tmp, strerror(errno));
                return ESP_FAIL;
            }
            *p = '/';
        }
    }
    /* 创建最后一级 */
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir(%s) 失败: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 递归删除目录及其所有内容（用于清理旧照片目录）。 */
static esp_err_t remove_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        /* 不存在视为已删除 */
        return ESP_OK;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* 普通文件：直接 unlink */
        if (unlink(path) != 0) {
            ESP_LOGE(TAG, "unlink(%s) 失败: %s", path, strerror(errno));
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    /* 目录：遍历并递归删除子项 */
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir(%s) 失败: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    struct dirent *ent = NULL;
    esp_err_t ret = ESP_OK;
    while ((ent = readdir(dir)) != NULL) {
        /* 跳过 "." 与 ".." */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[280] = {0};
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (remove_recursive(child) != ESP_OK) {
            ret = ESP_FAIL;
        }
    }
    closedir(dir);

    /* 子项清空后删除空目录 */
    if (rmdir(path) != 0) {
        ESP_LOGE(TAG, "rmdir(%s) 失败: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ret;
}

/* 判断路径是否为目录（含 stat 调用） */
static bool is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

/* ============================================================ */
/*                          公开 API                             */
/* ============================================================ */

esp_err_t sd_storage_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD 卡已挂载，跳过重复 init");
        return ESP_OK;
    }

    /* 1. 配置 SDSPI host（默认 SPI3_HOST + 自动 DMA 通道） */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    /* SDSPI_HOST_DEFAULT 已设置 .slot = SDSPI_DEFAULT_HOST(SPI3_HOST) 与 .max_freq_khz */

    /* 2. 初始化 SPI 总线，绑定 MOSI/MISO/SCK 引脚 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,   /* SDSPI 单次传输上限，对齐 SD 块大小 */
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_spi_bus_inited = (ret == ESP_OK);

    /* 3. 配置 SDSPI 设备（CS 引脚 + host_id） */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    /* 4. 挂载 FATFS 到 VFS，使能标准 C 文件 API（fopen/fwrite 等） */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,   /* 不自动格式化，保护已有照片 */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024, /* 16KB 簇，平衡空间利用率与碎片 */
    };

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡挂载失败: %s", esp_err_to_name(ret));
        if (s_spi_bus_inited) {
            spi_bus_free(host.slot);
            s_spi_bus_inited = false;
        }
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD 卡挂载成功：%s, 容量 %llu MB",
             SD_MOUNT_POINT,
             (unsigned long long)(s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t sd_storage_write_file(const char *path, const uint8_t *data, size_t len)
{
    if (path == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 自动创建父目录（如 /sdcard/photos/2024/01/xxx.jpg） */
    char dir[256] = {0};
    strncpy(dir, path, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (strlen(dir) > 0 && strcmp(dir, "/") != 0) {
            mkdir_p(dir);
        }
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s, wb) 失败: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    size_t written = (len > 0) ? fwrite(data, 1, len, f) : 0;
    int flush_ret = fflush(f);
    int close_ret = fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "fwrite 不完整：期望 %u，实际 %u",
                 (unsigned)len, (unsigned)written);
        return ESP_FAIL;
    }
    if (flush_ret != 0 || close_ret != 0) {
        ESP_LOGE(TAG, "文件关闭/刷新失败: %s", strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sd_storage_read_file(const char *path, uint8_t **out_buf, size_t *out_len)
{
    if (path == NULL || out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s, rb) 失败: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    /* 取文件大小 */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    if (buf == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "malloc(%ld) 失败", size);
        return ESP_ERR_NO_MEM;
    }

    size_t read = (size > 0) ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    if (read != (size_t)size) {
        ESP_LOGE(TAG, "fread 不完整：期望 %ld，实际 %u", size, (unsigned)read);
        free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    *out_len = (size_t)size;
    return ESP_OK;
}

esp_err_t sd_storage_mkdir(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return mkdir_p(path);
}

/* 字符串比较函数，供 qsort 按字典序排序目录名 */
static int cmp_str(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

esp_err_t sd_storage_list_dir(const char *path, char ***out_list, size_t *out_count)
{
    if (path == NULL || out_list == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_list = NULL;
    *out_count = 0;

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir(%s) 失败: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    /* 仅收集子目录名；动态扩展数组 */
    char **list = NULL;
    size_t count = 0;
    size_t cap = 0;
    struct dirent *ent = NULL;
    esp_err_t ret = ESP_OK;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        /* 拼接完整路径判断是否为目录 */
        char child[280] = {0};
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (!is_directory(child)) {
            continue;
        }

        if (count == cap) {
            cap = (cap == 0) ? 8 : cap * 2;
            char **tmp = realloc(list, cap * sizeof(char *));
            if (tmp == NULL) {
                ESP_LOGE(TAG, "realloc 失败");
                ret = ESP_ERR_NO_MEM;
                break;
            }
            list = tmp;
        }

        list[count] = strdup(ent->d_name);
        if (list[count] == NULL) {
            ESP_LOGE(TAG, "strdup 失败");
            ret = ESP_ERR_NO_MEM;
            break;
        }
        count++;
    }
    closedir(dir);

    if (ret != ESP_OK) {
        /* 失败时释放已分配资源 */
        for (size_t i = 0; i < count; i++) {
            free(list[i]);
        }
        free(list);
        return ret;
    }

    /* 按字典序排序 */
    if (count > 1) {
        qsort(list, count, sizeof(char *), cmp_str);
    }

    *out_list = list;
    *out_count = count;
    return ESP_OK;
}

esp_err_t sd_storage_get_free_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (free_bytes == NULL || total_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct statvfs stat = {0};
    if (statvfs(SD_MOUNT_POINT, &stat) != 0) {
        ESP_LOGE(TAG, "statvfs(%s) 失败: %s", SD_MOUNT_POINT, strerror(errno));
        return ESP_FAIL;
    }
    *free_bytes = (uint64_t)stat.f_bavail * stat.f_frsize;
    *total_bytes = (uint64_t)stat.f_blocks * stat.f_frsize;
    return ESP_OK;
}

esp_err_t sd_storage_delete_path(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return remove_recursive(path);
}

esp_err_t sd_storage_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdmmc_unmount(SD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡卸载失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_card = NULL;
    s_mounted = false;

    /* 释放 SPI 总线（仅当由本组件初始化时） */
    if (s_spi_bus_inited) {
        spi_bus_free(SDSPI_DEFAULT_HOST);
        s_spi_bus_inited = false;
    }

    ESP_LOGI(TAG, "SD 卡已卸载");
    return ESP_OK;
}
