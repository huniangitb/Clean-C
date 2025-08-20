// gc_trim.h
#ifndef GC_TRIM_H
#define GC_TRIM_H

#include <pthread.h>

typedef struct {
    long reclaimed_segments;
    double trimmed_mb;
} GCTrimStats;

// --- 新的 API ---

// 初始化 GC/Trim 模块 (在主程序启动时调用一次)
int gc_trim_init();

// 手动触发一次 GC (会动态创建一个线程)
void trigger_gc_once();

// 手动停止当前正在进行的 GC
void stop_current_gc();

// 检查 GC 线程当前是否正在活跃地执行 GC 操作
int is_gc_active();

// 获取并清零最新的统计数据
GCTrimStats get_gc_trim_stats();

#endif // GC_TRIM_H