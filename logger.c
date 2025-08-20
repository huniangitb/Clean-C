#define _GNU_SOURCE
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static FILE *g_log_file = NULL;
static int g_debug_level = 1;
static char g_log_filename[256];

static int ensure_log_file_is_open() {
    if (g_log_file != NULL) {
        // 检查文件描述符是否仍然有效
        int fd = fileno(g_log_file);
        if (fstat(fd, &(struct stat){0}) != 0) {
            fclose(g_log_file);
            g_log_file = NULL;
        }
    }

    if (g_log_file == NULL) {
        g_log_file = fopen(g_log_filename, "a");
        if (g_log_file == NULL) {
            fprintf(stderr, "致命错误: 无法打开日志文件 '%s': %s\n", g_log_filename, strerror(errno));
            return 0;
        }
        setvbuf(g_log_file, NULL, _IOLBF, BUFSIZ);
    }
    return 1;
}

int log_init(const char* log_filename, int level) {
    strncpy(g_log_filename, log_filename, sizeof(g_log_filename) - 1);
    g_log_filename[sizeof(g_log_filename) - 1] = '\0';
    g_debug_level = level;

    if (!ensure_log_file_is_open()) {
        return -1;
    }
    
    time_t now = time(NULL);
    char time_buf[30];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(g_log_file, "\n[%s] 日志系统已初始化\n", time_buf);
    fflush(g_log_file);
    return 0;
}

void log_cleanup() {
    if (g_log_file) {
        log_message(1, "日志系统正在关闭。");
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_set_level(int level) {
    g_debug_level = level;
}

void log_message(int level, const char *format, ...) {
    if (g_debug_level < level) return;
    if (!ensure_log_file_is_open()) {
        // 如果无法打开日志文件，则将严重错误打印到 stderr
        if (level <= 1) {
            fprintf(stderr, "日志文件错误，消息输出到 stderr: ");
            va_list args;
            va_start(args, format);
            vfprintf(stderr, format, args);
            va_end(args);
            fprintf(stderr, "\n");
        }
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(g_log_file, format, args);
    va_end(args);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);
}

void log_check_and_rotate(long max_size) {
    if (!g_log_file) return;

    struct stat statbuf;
    if (lstat(g_log_filename, &statbuf) == 0) {
        if (statbuf.st_size > max_size) {
            log_message(1, "日志文件大小 (%lld bytes) 超出限制 (%ld bytes)，开始轮转...", (long long)statbuf.st_size, max_size);
            
            fclose(g_log_file);
            g_log_file = NULL;

            char old_filename[300];
            snprintf(old_filename, sizeof(old_filename), "%s.old", g_log_filename);
            if (rename(g_log_filename, old_filename) != 0) {
                fprintf(stderr, "警告: 轮转日志文件失败: %s\n", strerror(errno));
            }
            
            // 重新打开文件
            ensure_log_file_is_open();
        }
    } else if (errno != ENOENT) {
        log_message(1, "警告: 检查日志大小时无法获取 '%s' 状态: %s", g_log_filename, strerror(errno));
    }
}