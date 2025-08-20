#define _GNU_SOURCE
#include "gc_trim.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/wait.h>

// --- 全局变量 ---
// 统计数据和其互斥锁
static GCTrimStats g_stats = {0, 0.0};
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// 控制标志
static atomic_int g_manual_stop = 0; // 手动停止当前 GC 的标志
static atomic_int g_is_active = 0;   // 是否有 GC 线程正在活跃执行

// F2FS sysfs 路径 (由 gc_trim_init 初始化)
static char g_sysfs_path[PATH_MAX] = {0};

// --- 辅助函数 ---

// 一个更安全地替代 popen 的函数，用于在多线程程序中执行命令并读取其输出
// 注意：这个函数在 gc_trim_init 中使用，而不是在 GC 工作线程中
static int exec_and_read(const char *command, char *output, size_t output_size) {
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) == -1) {
        log_message(0, "GC/Trim: pipe() 失败: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid == -1) {
        log_message(0, "GC/Trim: fork() 失败: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) { // 子进程
        close(pipefd[0]); // 关闭读端
        dup2(pipefd[1], STDOUT_FILENO); // 将标准输出重定向到管道的写端
        close(pipefd[1]); // 关闭不再需要的管道写端副本

        // 执行命令
        execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
        // 如果 execl 返回，说明出错了
        _exit(127); // 使用 _exit 避免刷新缓冲区和调用atexit处理函数
    } else { // 父进程
        close(pipefd[1]); // 关闭写端

        ssize_t bytes_read = read(pipefd[0], output, output_size - 1);
        
        close(pipefd[0]); // 关闭读端

        int status;
        waitpid(pid, &status, 0); // 等待子进程结束

        if (bytes_read > 0) {
            output[bytes_read] = '\0';
            return 0; // 成功
        }
    }
    return -1; // 失败
}


static long read_sysfs_file_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { return -1; }
    long val = -1;
    if (fscanf(f, "%ld", &val) != 1) { val = -1; }
    fclose(f);
    return val;
}

static int write_sysfs_file_string(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    if (!f) { return -1; }
    int result = fprintf(f, "%s", value);
    fclose(f);
    return (result > 0) ? 0 : -1;
}

static int get_cpu_cores() {
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) { return 1; }
    char *line = NULL; size_t len = 0; int cores = 0;
    while (getline(&line, &len, fp) != -1) { if (strncmp(line, "processor", 9) == 0) { cores++; } }
    free(line); fclose(fp); return (cores > 0) ? cores : 1;
}

static double get_cpu_usage() {
    long prev_user, prev_nice, prev_system, prev_idle, prev_iowait, prev_irq, prev_softirq;
    long curr_user, curr_nice, curr_system, curr_idle, curr_iowait, curr_irq, curr_softirq;
    long prev_total, curr_total, total_diff, idle_diff; double usage = 0.0; FILE *fp;
    fp = fopen("/proc/stat", "r");
    if (!fp) return 0.0;
    if (fscanf(fp, "cpu %ld %ld %ld %ld %ld %ld %ld", &prev_user, &prev_nice, &prev_system, &prev_idle, &prev_iowait, &prev_irq, &prev_softirq) < 7) { fclose(fp); return 0.0; }
    fclose(fp); sleep(1);
    fp = fopen("/proc/stat", "r");
    if (!fp) return 0.0;
    if (fscanf(fp, "cpu %ld %ld %ld %ld %ld %ld %ld", &curr_user, &curr_nice, &curr_system, &curr_idle, &curr_iowait, &curr_irq, &curr_softirq) < 7) { fclose(fp); return 0.0; }
    fclose(fp);
    prev_total = prev_user + prev_nice + prev_system + prev_idle + prev_iowait + prev_irq + prev_softirq;
    curr_total = curr_user + curr_nice + curr_system + curr_idle + curr_iowait + curr_irq + curr_softirq;
    total_diff = curr_total - prev_total; idle_diff = curr_idle - prev_idle;
    if (total_diff > 0) { usage = (double)(total_diff - idle_diff) * 100.0 / total_diff; }
    return usage;
}

static void trim_and_report(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { log_message(1, "GC/Trim: 无法打开路径 '%s' 进行 trim: %s", path, strerror(errno)); return; }
    struct fstrim_range range = { .start = 0, .len = ~0ULL, .minlen = 0 };
    log_message(2, "GC/Trim: 正在对 '%s' 执行 FITRIM ioctl...", path);
    if (ioctl(fd, FITRIM, &range) < 0) {
        log_message(1, "GC/Trim: FITRIM ioctl 在 '%s' 上失败: %s", path, strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    long long bytes_trimmed = range.len;
    double mb_trimmed = (double)bytes_trimmed / (1024.0 * 1024.0);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.trimmed_mb += mb_trimmed; // 累加 Trim 统计
    pthread_mutex_unlock(&g_stats_mutex);

    log_message(1, "GC/Trim: 已裁剪 %.2f MB from %s", mb_trimmed, path);
}

static void execute_gc(const char *sysfs_path) {
    char path_buffer[PATH_MAX];
    long initial_dirty_segs = -1; // 初始化为 -1，表示未成功获取

    snprintf(path_buffer, sizeof(path_buffer), "%s/dirty_segments", sysfs_path);
    initial_dirty_segs = read_sysfs_file_long(path_buffer);
    if (initial_dirty_segs < 256) {
        log_message(1, "GC/Trim: 脏段 (%ld) 低于阈值 256，跳过 GC。", initial_dirty_segs);
        return;
    }

    snprintf(path_buffer, sizeof(path_buffer), "%s/gc_urgent", sysfs_path);
    write_sysfs_file_string(path_buffer, "1");
    log_message(1, "GC/Trim: 开始 GC，初始脏段: %ld", initial_dirty_segs);
    int cores = get_cpu_cores();
    time_t last_progress_time = time(NULL);
    long last_dirty_count = initial_dirty_segs;
    const int no_progress_timeout = 60;

    while (!atomic_load(&g_manual_stop)) { // 循环直到手动停止
        long battery = read_sysfs_file_long("/sys/class/power_supply/battery/capacity");
        if (battery == -1) battery = 100;
        double cpu = get_cpu_usage();
        int cpu_ok = (cpu < (50.0 * cores));
        int battery_ok = (battery > 40);

        if (battery_ok && cpu_ok) {
            snprintf(path_buffer, sizeof(path_buffer), "%s/gc_urgent", sysfs_path);
            long current_gc_urgent = read_sysfs_file_long(path_buffer);
            if (current_gc_urgent == 0 || (strstr(path_buffer, "mifs") && current_gc_urgent == 2)) {
                 write_sysfs_file_string(path_buffer, "1");
            }
            snprintf(path_buffer, sizeof(path_buffer), "%s/dirty_segments", sysfs_path);
            long current_dirty = read_sysfs_file_long(path_buffer);
            if (current_dirty < 200) { // GC 完成条件
                long reclaimed = initial_dirty_segs - current_dirty;
                pthread_mutex_lock(&g_stats_mutex);
                g_stats.reclaimed_segments += reclaimed; // 累加回收段数
                pthread_mutex_unlock(&g_stats_mutex);
                char time_str[30]; time_t now = time(NULL);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
                log_message(1, "GC/Trim: GC 完成 [%s]，已回收 %ld 个脏段。当前脏段: %ld", time_str, reclaimed, current_dirty);
                goto gc_cleanup;
            }
            if (current_dirty < last_dirty_count) {
                last_progress_time = time(NULL);
                last_dirty_count = current_dirty;
                log_message(2, "GC/Trim: 进展... 当前脏段: %ld", current_dirty);
            } else {
                if (difftime(time(NULL), last_progress_time) > no_progress_timeout) {
                    long reclaimed = initial_dirty_segs - current_dirty;
                    pthread_mutex_lock(&g_stats_mutex);
                    g_stats.reclaimed_segments += reclaimed; // 累加回收段数
                    pthread_mutex_unlock(&g_stats_mutex);
                    char time_str[30]; time_t now = time(NULL);
                    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
                    log_message(1, "GC/Trim: GC 因 %d 秒无进展而超时退出 [%s]。已回收 %ld 个脏段。当前脏段: %ld", no_progress_timeout, time_str, reclaimed, current_dirty);
                    goto gc_cleanup;
                }
            }
            sleep(5);
        } else {
            snprintf(path_buffer, sizeof(path_buffer), "%s/gc_urgent", sysfs_path);
            write_sysfs_file_string(path_buffer, "0");
            if (!battery_ok) log_message(1, "GC/Trim: 暂停 GC，电量低: %ld%%", battery);
            if (!cpu_ok) log_message(1, "GC/Trim: 暂停 GC，CPU 占用高: %.2f%%", cpu);
            while (!atomic_load(&g_manual_stop)) { // 循环等待条件恢复
                battery = read_sysfs_file_long("/sys/class/power_supply/battery/capacity");
                if (battery == -1) battery = 100;
                cpu = get_cpu_usage();
                cpu_ok = (cpu < (50.0 * cores));
                battery_ok = (battery > 40);
                if (battery_ok && cpu_ok) {
                    log_message(1, "GC/Trim: 恢复条件满足。");
                    last_progress_time = time(NULL);
                    break;
                }
                sleep(60);
            }
        }
    }

    // 如果是手动停止，计算并累加已回收的段数
    if (atomic_load(&g_manual_stop)) {
        log_message(1, "GC/Trim: GC 被手动停止。");
        snprintf(path_buffer, sizeof(path_buffer), "%s/dirty_segments", sysfs_path);
        long final_dirty_segs = read_sysfs_file_long(path_buffer);
        if (initial_dirty_segs != -1 && final_dirty_segs != -1) {
            long reclaimed = initial_dirty_segs - final_dirty_segs;
            if (reclaimed > 0) {
                pthread_mutex_lock(&g_stats_mutex);
                g_stats.reclaimed_segments += reclaimed; // 累加回收段数
                pthread_mutex_unlock(&g_stats_mutex);
                log_message(1, "GC/Trim: 手动停止前已回收 %ld 个脏段。", reclaimed);
            }
        }
    }

gc_cleanup:
    snprintf(path_buffer, sizeof(path_buffer), "%s/gc_urgent", sysfs_path);
    write_sysfs_file_string(path_buffer, "0"); // GC 结束后，将 gc_urgent 设为 0
}

// 新的 GC 工作线程函数 (动态创建)
static void* gc_worker_thread_func(void* arg __attribute__((unused))) {
    // 检查是否已经有另一个 GC 线程在运行
    if (atomic_exchange(&g_is_active, 1) == 1) {
        log_message(1, "GC/Trim: 已有一个 GC 任务正在运行，本次触发被忽略。");
        return NULL;
    }

    log_message(1, "GC/Trim: 新的 GC 工作线程已启动。");
    
    // 重置手动停止标志，确保本次 GC 不受上次停止命令的影响
    atomic_store(&g_manual_stop, 0);

    // 执行 GC
    execute_gc(g_sysfs_path);

    // 在执行 Trim 前检查停止标志
    if (!atomic_load(&g_manual_stop)) {
        trim_and_report("/data");
    } else {
        log_message(1, "GC/Trim: 由于收到停止命令，跳过 trim 操作。");
    }

    log_message(1, "GC/Trim: GC 工作线程已完成任务并退出。");
    
    // 清除活跃标志
    atomic_store(&g_is_active, 0);
    return NULL;
}

// 初始化 GC/Trim 模块，获取 sysfs 路径 (在主程序启动时调用一次)
int gc_trim_init() {
    char data_device[256] = {0};
    char command_output[256] = {0};
    
    // 使用 popen 获取设备名，因为这是在主线程启动时执行，不是在频繁创建的 GC 线程中
    FILE *pp = popen("getprop dev.mnt.dev.data", "r");
    if (pp) {
        if (fgets(command_output, sizeof(command_output), pp) != NULL) {
            strncpy(data_device, command_output, sizeof(data_device) - 1);
            data_device[strcspn(data_device, "\r\n")] = 0; // 移除换行符
        }
        pclose(pp);
    }

    if (strlen(data_device) == 0) {
        log_message(0, "GC/Trim: 初始化失败，无法获取 dev.mnt.dev.data。");
        return -1;
    }

    // 尝试 F2FS 或 MIFS 的 sysfs 路径
    snprintf(g_sysfs_path, sizeof(g_sysfs_path), "/sys/fs/f2fs/%s", data_device);
    struct stat st;
    if (stat(g_sysfs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(g_sysfs_path, sizeof(g_sysfs_path), "/sys/fs/mifs/%s", data_device);
        if (stat(g_sysfs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            log_message(0, "GC/Trim: 初始化失败，f2fs 或 mifs sysfs 路径不存在。");
            g_sysfs_path[0] = '\0'; // 清空路径，表示初始化失败
            return -1;
        }
    }

    log_message(1, "GC/Trim: 模块已初始化，sysfs 路径为 '%s'。", g_sysfs_path);
    return 0;
}

// 手动触发一次 GC (会动态创建一个线程)
void trigger_gc_once() {
    log_message(1, "GC/Trim: 收到手动触发命令。");

    // 如果 GC/Trim 模块未成功初始化，则无法触发 GC
    if (g_sysfs_path[0] == '\0') {
        log_message(1, "GC/Trim: 模块未正确初始化，无法触发 GC。");
        return;
    }

    pthread_t tid;
    pthread_attr_t attr;

    // 初始化线程属性为分离状态 (detached state)
    // 分离线程在退出时会自动释放其资源，无需 pthread_join
    if (pthread_attr_init(&attr) != 0) {
        log_message(0, "GC/Trim: pthread_attr_init 失败。");
        return;
    }
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        log_message(0, "GC/Trim: pthread_attr_setdetachstate 失败。");
        pthread_attr_destroy(&attr);
        return;
    }

    // 创建分离线程来执行 GC 任务
    if (pthread_create(&tid, &attr, gc_worker_thread_func, NULL) != 0) {
        log_message(0, "GC/Trim: 无法创建 GC 工作线程: %s", strerror(errno));
    }

    pthread_attr_destroy(&attr); // 销毁线程属性对象
}

// 手动停止当前正在进行的 GC
void stop_current_gc() {
    log_message(1, "GC/Trim: 收到手动停止命令。");
    atomic_store(&g_manual_stop, 1); // 设置手动停止标志
}

// 检查 GC 线程当前是否正在活跃地执行 GC 操作
int is_gc_active() {
    return atomic_load(&g_is_active);
}

// 获取并清零最新的统计数据
GCTrimStats get_gc_trim_stats() {
    GCTrimStats current_stats;
    pthread_mutex_lock(&g_stats_mutex);
    current_stats = g_stats;
    // 读取后清零，以便下次统计周期重新开始累加
    g_stats.reclaimed_segments = 0;
    g_stats.trimmed_mb = 0.0;
    pthread_mutex_unlock(&g_stats_mutex);
    return current_stats;
}