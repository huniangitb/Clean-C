#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

// 初始化日志系统
int log_init(const char* log_filename, int level);

// 清理并关闭日志系统
void log_cleanup();

// 设置日志记录级别 (0: 错误, 1: 信息, 2: 调试)
void log_set_level(int level);

// 记录日志消息
void log_message(int level, const char *format, ...);

// 检查并根据大小轮转日志文件
void log_check_and_rotate(long max_size);

#endif // LOGGER_H