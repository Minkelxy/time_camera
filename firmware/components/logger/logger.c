#include "logger.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sd_storage.h"

static const char *TAG = "logger";

/* ---------- 常量定义 ---------- */

/*! 日志文件大小阈值：1MB（1024*1024 字节），超过则滚动 */
#define LOG_FILE_MAX_SIZE  (1024 * 1024)

/*! 时间戳缓冲区长度："YYYY-MM-DD HH:MM:SS" + '\0' = 20 */
#define TIMESTAMP_BUF_LEN  20

/*! 日期缓冲区长度："YYYYMMDD" + 余量 */
#define DATE_BUF_LEN       16

/*! 日志目录路径最大长度 */
#define LOG_DIR_MAX_LEN    64

/*! 日志文件路径最大长度 */
#define FILEPATH_MAX_LEN   96

/*! 格式化消息缓冲区长度 */
#define MSG_BUF_LEN        256

/*! 完整日志行缓冲区长度 */
#define LINE_BUF_LEN       512

/*! 互斥锁等待超时（毫秒） */
#define MUTEX_TIMEOUT_MS   1000

/* ---------- 静态状态变量 ---------- */

static char              s_log_dir[LOG_DIR_MAX_LEN] = {0};    /* 日志目录 */
static SemaphoreHandle_t s_mutex = NULL;                      /* 文件写入互斥锁 */
static char              s_current_date[DATE_BUF_LEN] = {0}; /* 当前日期 YYYYMMDD */
static size_t            s_current_file_size = 0;            /* 当前文件大小（字节） */
static bool              s_initialized = false;              /* 初始化标志 */
static void              (*s_time_source)(struct tm *) = NULL; /* 时间源回调 */

/* ---------- 内部辅助函数 ---------- */

/**
 * @brief 获取日志级别对应的字符串
 */
static const char *level_to_string(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_WARN:  return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_INFO:
    default:              return "INFO";
    }
}

/**
 * @brief 获取当前时间
 *
 * 若已通过 logger_set_time_source 注入时间源回调，则使用回调（如 DS3231 RTC）；
 * 否则使用系统时间 localtime(time(NULL))。
 *
 * @param out 输出参数，填充 struct tm
 */
static void get_current_time(struct tm *out)
{
    if (out == NULL) {
        return;
    }
    if (s_time_source != NULL) {
        s_time_source(out);
    } else {
        time_t now = time(NULL);
        *out = *localtime(&now);
    }
}

/**
 * @brief 格式化时间戳 "YYYY-MM-DD HH:MM:SS"
 *
 * @param t   时间结构体
 * @param out 输出缓冲区
 * @param len 缓冲区长度
 */
static void format_timestamp(struct tm *t, char *out, size_t len)
{
    if (t == NULL || out == NULL || len == 0) {
        return;
    }
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}

/**
 * @brief 格式化日期 "YYYYMMDD"
 *
 * @param t   时间结构体
 * @param out 输出缓冲区
 * @param len 缓冲区长度
 */
static void format_date(struct tm *t, char *out, size_t len)
{
    if (t == NULL || out == NULL || len == 0) {
        return;
    }
    snprintf(out, len, "%04d%02d%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
}

/**
 * @brief 检查并执行日志文件滚动
 *
 * 使用 stat() 获取文件大小（避免 fopen+fseek 的性能开销）。
 * 当文件大小超过 LOG_FILE_MAX_SIZE（1MB）时，将当前文件重命名为
 * YYYYMMDD_1.log、YYYYMMDD_2.log...（查找第一个不存在的序号），
 * 后续 fopen("a") 会自动创建新的 YYYYMMDD.log。
 *
 * @param filepath 当前日志文件路径
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_FAIL 重命名失败
 */
static esp_err_t check_and_rotate(const char *filepath)
{
    if (filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 使用 stat() 获取文件大小 */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        /* 文件不存在（首次写入或已滚动），大小为 0 */
        s_current_file_size = 0;
        return ESP_OK;
    }

    s_current_file_size = (size_t)st.st_size;

    /* 未超过阈值，无需滚动 */
    if (s_current_file_size <= LOG_FILE_MAX_SIZE) {
        return ESP_OK;
    }

    /* 超过阈值：查找第一个不存在的序号 N，重命名为 YYYYMMDD_N.log */
    char rotated_path[FILEPATH_MAX_LEN];
    int index = 1;
    while (index < 1000) {
        snprintf(rotated_path, sizeof(rotated_path), "%s/%s_%d.log",
                 s_log_dir, s_current_date, index);
        struct stat tmp;
        if (stat(rotated_path, &tmp) != 0) {
            break;  /* 该序号文件不存在，使用此序号 */
        }
        index++;
    }

    if (index >= 1000) {
        ESP_LOGE(TAG, "日志滚动失败：序号已达上限 (%d)", index);
        return ESP_FAIL;
    }

    /* 重命名当前日志文件 */
    if (rename(filepath, rotated_path) != 0) {
        ESP_LOGE(TAG, "日志滚动失败：rename(%s -> %s) errno=%d",
                 filepath, rotated_path, errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "日志滚动：%s -> %s", filepath, rotated_path);
    s_current_file_size = 0;
    return ESP_OK;
}

/* ---------- 公共 API 实现 ---------- */

esp_err_t logger_init(const char *log_dir)
{
    if (log_dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 防止重复初始化 */
    if (s_initialized) {
        ESP_LOGW(TAG, "logger 已初始化，忽略重复调用");
        return ESP_OK;
    }

    /* 保存日志目录 */
    if (strlen(log_dir) >= LOG_DIR_MAX_LEN) {
        ESP_LOGE(TAG, "日志目录路径过长 (>= %d)", LOG_DIR_MAX_LEN);
        return ESP_ERR_INVALID_SIZE;
    }
    strncpy(s_log_dir, log_dir, LOG_DIR_MAX_LEN - 1);
    s_log_dir[LOG_DIR_MAX_LEN - 1] = '\0';

    /* 调用 sd_storage 确保日志目录存在 */
    esp_err_t ret = sd_storage_mkdir(s_log_dir);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建日志目录失败：%s (0x%x)", s_log_dir, ret);
        return ret;
    }

    /* 创建互斥锁保护并发文件写入 */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    /* 初始化内部状态：当前日期、文件大小、时间源 */
    struct tm now;
    memset(&now, 0, sizeof(now));
    get_current_time(&now);
    format_date(&now, s_current_date, DATE_BUF_LEN);
    s_current_file_size = 0;
    s_time_source = NULL;

    s_initialized = true;
    ESP_LOGI(TAG, "日志系统初始化完成：目录=%s 当前日期=%s", s_log_dir, s_current_date);
    return ESP_OK;
}

esp_err_t logger_log(log_level_t level, const char *event, const char *fmt, ...)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event == NULL || fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先格式化用户消息（使用栈缓冲区，在互斥锁外完成） */
    char msg_buf[MSG_BUF_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    const char *lvl_str = level_to_string(level);
    esp_err_t ret = ESP_OK;

    /* 获取互斥锁（线程安全） */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "获取互斥锁超时 (%dms)", MUTEX_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    do {
        /* 获取当前时间 */
        struct tm now;
        memset(&now, 0, sizeof(now));
        get_current_time(&now);

        /* 格式化时间戳与日期 */
        char timestamp[TIMESTAMP_BUF_LEN];
        char date[DATE_BUF_LEN];
        format_timestamp(&now, timestamp, sizeof(timestamp));
        format_date(&now, date, sizeof(date));

        /* 跨日检查：日期变化则切换到新日志文件 */
        if (strncmp(date, s_current_date, DATE_BUF_LEN) != 0) {
            strncpy(s_current_date, date, DATE_BUF_LEN - 1);
            s_current_date[DATE_BUF_LEN - 1] = '\0';
            s_current_file_size = 0;
            ESP_LOGI(TAG, "日期切换 → %s，使用新日志文件", s_current_date);
        }

        /* 组装完整日志行：[YYYY-MM-DD HH:MM:SS] [LEVEL] [event] message\n */
        char line_buf[LINE_BUF_LEN];
        int line_len = snprintf(line_buf, sizeof(line_buf),
                                "[%s] [%s] [%s] %s\n",
                                timestamp, lvl_str, event, msg_buf);
        if (line_len < 0) {
            ESP_LOGE(TAG, "格式化日志行失败");
            ret = ESP_FAIL;
            break;
        }

        /* 构建日志文件路径：{log_dir}/YYYYMMDD.log */
        char filepath[FILEPATH_MAX_LEN];
        snprintf(filepath, sizeof(filepath), "%s/%s.log", s_log_dir, s_current_date);

        /* 检查文件大小并执行滚动（失败不阻断写入，错误已在内部记录） */
        (void)check_and_rotate(filepath);

        /* 以 append 模式打开文件、写入、关闭 */
        FILE *fp = fopen(filepath, "a");
        if (fp == NULL) {
            ESP_LOGE(TAG, "打开日志文件失败：%s (errno=%d)", filepath, errno);
            ret = ESP_FAIL;
            break;
        }

        /* 计算实际写入长度（处理 snprintf 截断情况） */
        size_t write_len;
        if ((size_t)line_len < sizeof(line_buf)) {
            write_len = (size_t)line_len;
        } else {
            write_len = sizeof(line_buf) - 1;  /* 被截断，不含 '\0' */
        }

        size_t written = fwrite(line_buf, 1, write_len, fp);
        if (written != write_len) {
            ESP_LOGE(TAG, "写入日志文件失败：%s (written=%u, expect=%u)",
                     filepath, (unsigned)written, (unsigned)write_len);
            ret = ESP_FAIL;
        } else {
            /* 更新当前文件大小 */
            s_current_file_size += written;
        }

        fflush(fp);
        fclose(fp);
    } while (0);

    /* 释放互斥锁 */
    xSemaphoreGive(s_mutex);

    /* 同时通过 ESP_LOG 输出到串口（便于调试） */
    switch (level) {
    case LOG_LEVEL_WARN:
        ESP_LOGW(TAG, "[%s] %s", event, msg_buf);
        break;
    case LOG_LEVEL_ERROR:
        ESP_LOGE(TAG, "[%s] %s", event, msg_buf);
        break;
    case LOG_LEVEL_INFO:
    default:
        ESP_LOGI(TAG, "[%s] %s", event, msg_buf);
        break;
    }

    return ret;
}

esp_err_t logger_set_time_source(void (*get_time)(struct tm *))
{
    s_time_source = get_time;
    ESP_LOGI(TAG, "时间源回调已%s", get_time != NULL ? "设置" : "清除");
    return ESP_OK;
}

esp_err_t logger_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* 每次写入后已 fclose+fflush，无持久缓冲需刷新 */

    /* 删除互斥锁 */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    /* 清除时间源回调 */
    s_time_source = NULL;

    /* 重置内部状态，标记未初始化 */
    s_initialized = false;
    s_current_date[0] = '\0';
    s_current_file_size = 0;
    s_log_dir[0] = '\0';

    ESP_LOGI(TAG, "日志系统已反初始化");
    return ESP_OK;
}
