#include "stats_manager.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <cjson/cJSON.h>
#include <unistd.h>
#include <limits.h>
#define HASH_TABLE_SIZE 257

typedef struct AppCleanStatNode {
    char *packageName;
    long long totalBytes;
    struct AppCleanStatNode *next;
} AppCleanStatNode;

typedef struct {
    AppCleanStatNode **buckets;
    int size;
} AppCleanHashTable;

static struct {
    int files_deleted;
    int dirs_deleted;
    long long bytes_deleted_this_cycle;
    AppCleanHashTable *app_stats_table;
    char json_log_path[PATH_MAX];
    char app_log_path[PATH_MAX];
    long long json_log_max_size;
} g_stats;

static unsigned long hash_function(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static void update_app_clean_stats(const char *path, long long size) {
    const char *match_pos = NULL;
    size_t prefix_len = 0;

    if ((match_pos = strstr(path, APP_CLEAN_PATH_USER))) {
        prefix_len = strlen(APP_CLEAN_PATH_USER);
    } else if ((match_pos = strstr(path, APP_CLEAN_PATH_MEDIA))) {
        prefix_len = strlen(APP_CLEAN_PATH_MEDIA);
    } else {
        return;
    }

    const char *start = match_pos + prefix_len;
    const char *end = strchr(start, '/');
    if (!end || start == end) return;

    size_t pkg_len = end - start;
    char package_name[256];
    if (pkg_len >= sizeof(package_name)) {
        log_message(1, "警告: 检测到过长的应用包名，跳过统计: %.*s", (int)pkg_len, start);
        return;
    }
    strncpy(package_name, start, pkg_len);
    package_name[pkg_len] = '\0';

    unsigned long index = hash_function(package_name) % g_stats.app_stats_table->size;
    AppCleanStatNode *node = g_stats.app_stats_table->buckets[index];
    while (node != NULL) {
        if (strcmp(node->packageName, package_name) == 0) {
            node->totalBytes += size;
            return;
        }
        node = node->next;
    }

    AppCleanStatNode *new_node = malloc(sizeof(AppCleanStatNode));
    if (!new_node) return;
    new_node->packageName = strdup(package_name);
    if (!new_node->packageName) { free(new_node); return; }
    new_node->totalBytes = size;
    new_node->next = g_stats.app_stats_table->buckets[index];
    g_stats.app_stats_table->buckets[index] = new_node;
}

static void log_app_cleanup_summary_text() {
    if (!g_stats.app_stats_table) return;
    FILE* app_log_file = fopen(g_stats.app_log_path, "w");
    if (!app_log_file) {
        log_message(1, "错误: 无法打开或创建 %s 文件进行写入: %s", g_stats.app_log_path, strerror(errno));
        return;
    }
    time_t now = time(NULL);
    char time_buf[30];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(app_log_file, "--- 应用垃圾清理统计 [%s] ---\n", time_buf);
    int count = 0;
    for (int i = 0; i < g_stats.app_stats_table->size; i++) {
        AppCleanStatNode *node = g_stats.app_stats_table->buckets[i];
        while (node != NULL) {
            double deleted_mb = (node->totalBytes > 0) ? (double)node->totalBytes / (1024.0 * 1024.0) : 0.0;
            fprintf(app_log_file, "包名: %-40s | 已清理: %10.2f MB (%lld Bytes)\n", node->packageName, deleted_mb, node->totalBytes);
            node = node->next;
            count++;
        }
    }
    if (count == 0) {
        fprintf(app_log_file, "本次运行未清理任何符合条件的应用垃圾。\n");
    }
    fprintf(app_log_file, "--- 统计结束 ---\n");
    fclose(app_log_file);
}

static void check_and_rotate_json_log() {
    struct stat statbuf;
    if (lstat(g_stats.json_log_path, &statbuf) == 0) {
        if (statbuf.st_size > g_stats.json_log_max_size) {
            log_message(1, "STATS: JSON 日志文件大小 (%lld bytes) 超出限制 (%lld bytes)，开始轮转...", (long long)statbuf.st_size, g_stats.json_log_max_size);
            char old_path[PATH_MAX + 4];
            snprintf(old_path, sizeof(old_path), "%s.old", g_stats.json_log_path);
            if (rename(g_stats.json_log_path, old_path) != 0) {
                log_message(0, "STATS: 错误: 轮转 JSON 日志文件失败: %s", strerror(errno));
            }
        }
    } else if (errno != ENOENT) {
        log_message(1, "STATS: 警告: 检查 JSON 日志大小时无法获取 '%s' 状态: %s", g_stats.json_log_path, strerror(errno));
    }
}

static void log_stats_as_json(double duration, GCTrimStats gc_stats, int gc_enabled) {
    if (!g_stats.app_stats_table) {
        log_message(1, "STATS: 跳过 JSON 日志，因为 app_stats_table 未初始化。");
        return;
    }

    // --- 新增调试日志 ---
    char full_path[PATH_MAX];
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        snprintf(full_path, sizeof(full_path), "%s/%s", cwd, g_stats.json_log_path);
    } else {
        strncpy(full_path, g_stats.json_log_path, PATH_MAX);
        full_path[PATH_MAX - 1] = '\0';
    }
    log_message(2, "STATS: 准备写入 JSON 日志到: %s (相对路径: %s)", full_path, g_stats.json_log_path);
    // --- 调试日志结束 ---

    check_and_rotate_json_log();

    FILE *json_file = fopen(g_stats.json_log_path, "a");
    if (!json_file) {
        log_message(0, "STATS: 致命错误: 无法以追加模式打开或创建 %s 文件: %s", g_stats.json_log_path, strerror(errno));
        return;
    }
    log_message(2, "STATS: 成功打开 %s 文件。", g_stats.json_log_path);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        log_message(0, "STATS: 致命错误: cJSON_CreateObject() 失败。");
        fclose(json_file);
        return;
    }
    log_message(2, "STATS: cJSON 根对象创建成功。");

    time_t now = time(NULL);
    char time_buf[30];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    cJSON_AddStringToObject(root, "timestamp", time_buf);
    cJSON_AddNumberToObject(root, "cycle_duration_seconds", duration);

    // 全局统计
    cJSON *global_stats = cJSON_CreateObject();
    if (global_stats) {
        cJSON_AddNumberToObject(global_stats, "files_deleted", g_stats.files_deleted);
        cJSON_AddNumberToObject(global_stats, "dirs_deleted", g_stats.dirs_deleted);
        cJSON_AddNumberToObject(global_stats, "bytes_deleted", g_stats.bytes_deleted_this_cycle);
        cJSON_AddNumberToObject(global_stats, "megabytes_deleted", (double)g_stats.bytes_deleted_this_cycle / (1024.0 * 1024.0));
        cJSON_AddItemToObject(root, "global_stats", global_stats);
    }
    log_message(2, "STATS: 全局统计对象添加成功。");


    // GC/Trim 统计
    if (gc_enabled) {
        cJSON *gc_trim_stats = cJSON_CreateObject();
        if (gc_trim_stats) {
            cJSON_AddNumberToObject(gc_trim_stats, "reclaimed_segments", gc_stats.reclaimed_segments);
            cJSON_AddNumberToObject(gc_trim_stats, "trimmed_mb", gc_stats.trimmed_mb);
            cJSON_AddItemToObject(root, "gc_trim_stats", gc_trim_stats);
        }
        log_message(2, "STATS: GC/Trim 统计对象添加成功 (启用状态: %d)。", gc_enabled);
    } else {
        log_message(2, "STATS: GC/Trim 统计未启用，跳过添加。");
    }


    // 应用统计
    cJSON *app_stats_array = cJSON_CreateArray();
    if (app_stats_array) {
        int app_count = 0;
        for (int i = 0; i < g_stats.app_stats_table->size; i++) {
            AppCleanStatNode *node = g_stats.app_stats_table->buckets[i];
            while (node != NULL) {
                cJSON *app_stat_item = cJSON_CreateObject();
                if (app_stat_item) {
                    cJSON_AddStringToObject(app_stat_item, "package_name", node->packageName);
                    cJSON_AddNumberToObject(app_stat_item, "bytes_deleted", node->totalBytes);
                    cJSON_AddNumberToObject(app_stat_item, "megabytes_deleted", (double)node->totalBytes / (1024.0 * 1024.0));
                    cJSON_AddItemToArray(app_stats_array, app_stat_item);
                    app_count++;
                }
                node = node->next;
            }
        }
        cJSON_AddItemToObject(root, "app_stats", app_stats_array);
        log_message(2, "STATS: 已添加 %d 个应用统计项。", app_count);
    } else {
        log_message(0, "STATS: 致命错误: cJSON_CreateArray() 失败。");
    }


    char *json_string = cJSON_PrintUnformatted(root);
    if (!json_string) {
        log_message(0, "STATS: 致命错误: cJSON_PrintUnformatted() 返回 NULL。");
        cJSON_Delete(root);
        fclose(json_file);
        return;
    }
    log_message(2, "STATS: cJSON 字符串生成成功，长度: %zu", strlen(json_string));
    // log_message(2, "STATS: 生成的 JSON: %s", json_string); // 如果需要，可以取消这行注释来查看完整 JSON

    // 尝试写入文件
    if (fprintf(json_file, "%s\n", json_string) < 0) {
        log_message(0, "STATS: 致命错误: fprintf() 写入 JSON 文件失败: %s", strerror(errno));
    } else {
        log_message(2, "STATS: fprintf() 写入成功。");
    }
    
    // 强制刷新缓冲区，确保数据写入磁盘
    if (fflush(json_file) != 0) {
        log_message(0, "STATS: 致命错误: fflush() 写入 JSON 文件失败: %s", strerror(errno));
    } else {
        log_message(2, "STATS: fflush() 成功。");
    }

    free(json_string);
    cJSON_Delete(root);
    fclose(json_file);
    log_message(2, "STATS: JSON 日志已写入并关闭文件。");
}

int stats_init(const char* json_log_file, long long json_max_size, const char* app_log_file) {
    memset(&g_stats, 0, sizeof(g_stats));
    strncpy(g_stats.json_log_path, json_log_file, sizeof(g_stats.json_log_path) - 1);
    g_stats.json_log_path[sizeof(g_stats.json_log_path) - 1] = '\0'; // 确保 null 终止
    strncpy(g_stats.app_log_path, app_log_file, sizeof(g_stats.app_log_path) - 1);
    g_stats.app_log_path[sizeof(g_stats.app_log_path) - 1] = '\0'; // 确保 null 终止
    g_stats.json_log_max_size = json_max_size;

    g_stats.app_stats_table = malloc(sizeof(AppCleanHashTable));
    if (!g_stats.app_stats_table) {
        log_message(0, "致命错误: 无法为应用统计哈希表分配内存。");
        return -1;
    }
    g_stats.app_stats_table->size = HASH_TABLE_SIZE;
    g_stats.app_stats_table->buckets = calloc(g_stats.app_stats_table->size, sizeof(AppCleanStatNode*));
    if (!g_stats.app_stats_table->buckets) {
        log_message(0, "致命错误: 无法为应用统计哈希表桶分配内存。");
        free(g_stats.app_stats_table);
        g_stats.app_stats_table = NULL;
        return -1;
    }
    log_message(1, "STATS: 统计模块已初始化。");
    return 0;
}

void stats_reset_cycle() {
    g_stats.files_deleted = 0;
    g_stats.dirs_deleted = 0;
    g_stats.bytes_deleted_this_cycle = 0;

    if (!g_stats.app_stats_table) return;
    for (int i = 0; i < g_stats.app_stats_table->size; i++) {
        AppCleanStatNode *current = g_stats.app_stats_table->buckets[i];
        while (current != NULL) {
            AppCleanStatNode *temp = current;
            current = current->next;
            free(temp->packageName);
            free(temp);
        }
        g_stats.app_stats_table->buckets[i] = NULL;
    }
    log_message(2, "STATS: 统计数据已重置。");
}

void stats_cleanup() {
    if (!g_stats.app_stats_table) return;
    stats_reset_cycle(); // This also frees all nodes
    free(g_stats.app_stats_table->buckets);
    free(g_stats.app_stats_table);
    g_stats.app_stats_table = NULL;
    log_message(1, "STATS: 统计模块已清理。");
}

void stats_record_deletion(const char *path, long long size, int is_dir) {
    if (is_dir) {
        g_stats.dirs_deleted++;
    } else {
        g_stats.files_deleted++;
        g_stats.bytes_deleted_this_cycle += size;
        if (g_stats.app_stats_table) {
            update_app_clean_stats(path, size);
        }
    }
}

void stats_log_summary(double duration, GCTrimStats gc_stats, int gc_enabled) {
    log_message(1, "STATS: 正在生成统计摘要...");
    log_app_cleanup_summary_text(); // 文本日志
    log_stats_as_json(duration, gc_stats, gc_enabled); // JSON 日志

    double deleted_mb = (g_stats.bytes_deleted_this_cycle > 0) ? (double)g_stats.bytes_deleted_this_cycle / (1024.0 * 1024.0) : 0.0;
    log_message(1, "全局统计: 已删除垃圾 %.2f MB", deleted_mb);
    log_message(1, "全局统计: 已删除文件数: %d", g_stats.files_deleted);
    log_message(1, "全局统计: 已删除目录数: %d", g_stats.dirs_deleted);
}