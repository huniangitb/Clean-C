#include "config_manager.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

// 内部数据结构
static ConfigData g_config;
static time_t g_mtime_bl1 = 0;
static time_t g_mtime_bl2 = 0;
static time_t g_mtime_wl = 0;

static void free_array(char **array, int count) {
    if (!array) return;
    for (int i = 0; i < count; i++) {
        free(array[i]);
    }
    free(array);
}

static int read_file_to_array(const char *filename, char ***array) {
    if (!filename || !array) return 0;
    *array = NULL;
    FILE *file = fopen(filename, "r");
    if (!file) {
        log_message(0, "无法打开配置文件: %s, 原因: %s", filename, strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t line_cap = 0;
    int count = 0;
    int capacity = 10;
    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        log_message(0, "内存分配失败 (read_file_to_array)");
        fclose(file);
        free(line);
        return -1;
    }

    while (getline(&line, &line_cap, file) != -1) {
        char *start = line;
        while (*start && (*start == ' ' || *start == '\t')) start++;
        char *end = start + strlen(start) - 1;
        while (end >= start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
            *end = '\0';
            end--;
        }
        if (*start == '#' || *start == '\0') continue;

        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                log_message(0, "内存重分配失败 (read_file_to_array)");
                for (int j = 0; j < count; j++) free(temp_array[j]);
                free(temp_array);
                free(line);
                fclose(file);
                return -1;
            }
            temp_array = new_array;
        }
        temp_array[count] = strdup(start);
        if (!temp_array[count]) {
            log_message(0, "内存分配失败 (strdup)");
            for (int j = 0; j < count; j++) free(temp_array[j]);
            free(temp_array);
            free(line);
            fclose(file);
            return -1;
        }
        count++;
    }
    free(line);
    fclose(file);

    if (count == 0) {
        free(temp_array);
        temp_array = NULL;
        log_message(2, "配置文件 '%s' 为空或只包含注释/空行。", filename);
    } else {
        char **final_array = realloc(temp_array, sizeof(char*) * count);
        if (final_array) temp_array = final_array;
    }
    *array = temp_array;
    return count;
}

static time_t get_file_mtime(const char *filename) {
    struct stat statbuf;
    if (lstat(filename, &statbuf) == 0) {
        return statbuf.st_mtime;
    }
    if (errno != ENOENT) {
        log_message(1, "无法获取文件 '%s' 的状态信息: %s", filename, strerror(errno));
    }
    return (time_t)0;
}

void config_init() {
    memset(&g_config, 0, sizeof(ConfigData));
}

const ConfigData* config_load(const char *bl1_file, const char *bl2_file, const char *wl_file) {
    time_t current_mtime;
    int new_count;

    // 处理白名单
    current_mtime = get_file_mtime(wl_file);
    if (g_config.whitelist == NULL || (current_mtime != (time_t)0 && current_mtime != g_mtime_wl)) {
        log_message(1, "检测到白名单 '%s' 已更新或首次加载，重新加载...", wl_file);
        free_array(g_config.whitelist, g_config.wl_count);
        g_config.whitelist = NULL;
        new_count = read_file_to_array(wl_file, &g_config.whitelist);
        if (new_count >= 0) {
            g_config.wl_count = new_count;
            g_mtime_wl = current_mtime;
            log_message(1, "成功从 '%s' 加载 %d 条白名单规则。", wl_file, g_config.wl_count);
        } else {
            log_message(0, "从 '%s' 加载白名单规则失败。将尝试使用旧缓存（如果存在）。", wl_file);
            g_mtime_wl = (time_t)0;
            if (g_config.whitelist == NULL) return NULL; // 初始加载失败且无缓存，致命错误
        }
    }

    // 处理黑名单1
    current_mtime = get_file_mtime(bl1_file);
    if (g_config.blacklist1 == NULL || (current_mtime != (time_t)0 && current_mtime != g_mtime_bl1)) {
        log_message(1, "检测到黑名单1 '%s' 已更新或首次加载，重新加载...", bl1_file);
        free_array(g_config.blacklist1, g_config.bl1_count);
        g_config.blacklist1 = NULL;
        new_count = read_file_to_array(bl1_file, &g_config.blacklist1);
        if (new_count >= 0) {
            g_config.bl1_count = new_count;
            g_mtime_bl1 = current_mtime;
            log_message(1, "成功从 '%s' 加载 %d 条黑名单1规则。", bl1_file, g_config.bl1_count);
        } else {
            log_message(0, "从 '%s' 加载黑名单1规则失败。将尝试使用旧缓存（如果存在）。", bl1_file);
            g_mtime_bl1 = (time_t)0;
            if (g_config.blacklist1 == NULL) return NULL; // 初始加载失败且无缓存，致命错误
        }
    }

    // 处理黑名单2
    if (bl2_file) {
        current_mtime = get_file_mtime(bl2_file);
        if (g_config.blacklist2 == NULL || (current_mtime != (time_t)0 && current_mtime != g_mtime_bl2)) {
            log_message(1, "检测到黑名单2 '%s' 已更新或首次加载，重新加载...", bl2_file);
            free_array(g_config.blacklist2, g_config.bl2_count);
            g_config.blacklist2 = NULL;
            new_count = read_file_to_array(bl2_file, &g_config.blacklist2);
            if (new_count >= 0) {
                g_config.bl2_count = new_count;
                g_mtime_bl2 = current_mtime;
                log_message(1, "成功从 '%s' 加载 %d 条黑名单2规则。", bl2_file, g_config.bl2_count);
            } else {
                log_message(1, "警告: 从 '%s' 加载黑名单2规则失败。将尝试使用旧缓存。", bl2_file);
                g_mtime_bl2 = (time_t)0;
            }
        }
    } else {
        if (g_config.blacklist2 != NULL) {
            log_message(1, "黑名单2文件未指定或已移除，清除相关缓存。");
            free_array(g_config.blacklist2, g_config.bl2_count);
            g_config.blacklist2 = NULL;
            g_config.bl2_count = 0;
            g_mtime_bl2 = (time_t)0;
        }
    }

    return &g_config;
}

void config_free() {
    free_array(g_config.blacklist1, g_config.bl1_count);
    free_array(g_config.blacklist2, g_config.bl2_count);
    free_array(g_config.whitelist, g_config.wl_count);
    memset(&g_config, 0, sizeof(ConfigData));
}