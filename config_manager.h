#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <time.h>

typedef struct {
    char **blacklist1;
    int bl1_count;
    char **blacklist2;
    int bl2_count;
    char **whitelist;
    int wl_count;
} ConfigData;

// 初始化配置管理器
void config_init();

// 加载或重新加载所有配置文件（如果已修改）
// 返回一个指向包含最新配置数据的常量指针
const ConfigData* config_load(const char *bl1_file, const char *bl2_file, const char *wl_file);

// 释放配置管理器使用的所有资源
void config_free();

#endif // CONFIG_MANAGER_H