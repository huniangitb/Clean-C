#ifndef FILE_CLEANER_H
#define FILE_CLEANER_H

#include "config_manager.h"

// 处理黑名单1（无过期检查）
void cleaner_process_blacklist1(const ConfigData *config);

// 处理黑名单2（有过期检查）
void cleaner_process_blacklist2(const ConfigData *config, int days);

#endif // FILE_CLEANER_H