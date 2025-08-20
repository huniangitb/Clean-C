#ifndef STATS_MANAGER_H
#define STATS_MANAGER_H

#include "gc_trim.h"
#include <limits.h>

#define APP_CLEAN_PATH_USER "data/user/0/"
#define APP_CLEAN_PATH_MEDIA "data/media/0/Android/data/"

// 初始化统计管理器
int stats_init(const char* json_log_file, long long json_max_size, const char* app_log_file);

// 为新的清理周期重置统计数据
void stats_reset_cycle();

// 记录一次删除操作
void stats_record_deletion(const char *path, long long size, int is_dir);

// 生成并记录本周期的统计摘要（文本和JSON）
void stats_log_summary(double duration, GCTrimStats gc_stats, int gc_enabled);

// 清理统计管理器使用的所有资源
void stats_cleanup();

#endif // STATS_MANAGER_H