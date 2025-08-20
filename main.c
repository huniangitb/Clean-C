#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <time.h>

#include "logger.h"
#include "config_manager.h"
#include "stats_manager.h"
#include "file_cleaner.h"
#include "gc_trim.h"    // 包含 gc_trim 模块
#include "udp_server.h" // 包含 UDP 服务器模块

#define LOG_FILE "run.log"
#define APP_LOG_FILE "app-clean.log"
#define JSON_STATS_FILE "stats.json"
#define MAX_LOG_SIZE (256 * 1024)
#define DEFAULT_UDP_PORT 9038 // 默认 UDP 监听端口

static void print_usage(const char *prog_name) {
    fprintf(stderr, "用法: %s -1 <bl1_file> -w <wl_file> [选项]\n", prog_name);
    fprintf(stderr, "必选参数:\n");
    fprintf(stderr, "  -1, --blacklist1 <文件>   黑名单文件1 (无过期检查)\n");
    fprintf(stderr, "  -w, --whitelist <文件>    白名单文件\n");
    fprintf(stderr, "可选参数:\n");
    fprintf(stderr, "  -2, --blacklist2 <文件>   黑名单文件2 (需要 --days)\n");
    fprintf(stderr, "  -D, --days <天数>         为黑名单2设置的过期天数\n");
    fprintf(stderr, "  -s, --seconds <秒>        循环运行的间隔时间 (0=单次运行)\n");
    fprintf(stderr, "  -d, --debug <级别>        日志级别 (0=错误, 1=信息, 2=调试)\n");
    fprintf(stderr, "  -j, --json-log-size <KB>  JSON日志文件的最大大小 (KB)\n");
    fprintf(stderr, "  -g, --enable-gc           启用 F2FS GC/Trim 功能 (默认: 启用)\n"); // GC功能现在默认启用，但需要F2FS文件系统
    fprintf(stderr, "  -U, --enable-udp          启用 UDP 命令服务器 (默认: 禁用)\n"); // 修改为 -U
    fprintf(stderr, "  --udp-port <端口>         UDP 服务器监听端口 (默认: %d)\n", DEFAULT_UDP_PORT);
    fprintf(stderr, "特殊命令:\n");
    fprintf(stderr, "      --trigger-gc          手动触发一次 GC/Trim 并退出\n");
    fprintf(stderr, "      --stop-gc             向正在运行的 GC 进程发送停止信号并退出\n");
}

// 处理一次性 GC 触发的逻辑
static void handle_gc_trigger() {
    log_message(1, "执行一次性 GC 触发...");
    time_t start_time = time(NULL);

    // 初始化统计模块，用于记录本次 GC 的统计数据
    if (stats_init(JSON_STATS_FILE, 64 * 1024, APP_LOG_FILE) != 0) {
        log_message(0, "统计模块初始化失败，无法记录GC统计。");
        return;
    }
    
    // 初始化 GC/Trim 模块 (获取 F2FS sysfs 路径)
    if (gc_trim_init() != 0) {
        log_message(0, "GC/Trim 模块初始化失败，无法执行 GC。");
        stats_cleanup();
        return;
    }

    // 触发 GC
    trigger_gc_once();
    
    // 等待 GC 完成
    sleep(1); // 给予线程启动时间
    while(is_gc_active()) {
        log_message(2, "等待 GC 操作完成...");
        sleep(5); // 每5秒检查一次
    }
    
    // 获取 GC 统计数据
    GCTrimStats gc_stats = get_gc_trim_stats();
    
    time_t end_time = time(NULL);
    double duration = difftime(end_time, start_time);

    // 记录统计数据
    stats_log_summary(duration, gc_stats, 1); // 强制记录 GC 统计，因为是手动触发
    
    stats_cleanup(); // 清理统计模块资源
    log_message(1, "一次性 GC 完成，已记录统计数据。");
}


int main(int argc, char *argv[]) {
    // 切换到程序所在目录
    char *executable_path_copy = strdup(argv[0]);
    if (executable_path_copy) {
        char *dir = dirname(executable_path_copy);
        if (chdir(dir) != 0) {
            fprintf(stderr, "致命错误: 无法切换到程序目录 '%s': %s\n", dir, strerror(errno));
            free(executable_path_copy);
            return EXIT_FAILURE;
        }
        free(executable_path_copy);
    }

    static struct option long_options[] = {
        {"blacklist1",    required_argument, 0, '1'},
        {"blacklist2",    required_argument, 0, '2'},
        {"whitelist",     required_argument, 0, 'w'},
        {"days",          required_argument, 0, 'D'},
        {"seconds",       required_argument, 0, 's'},
        {"debug",         required_argument, 0, 'd'},
        {"json-log-size", required_argument, 0, 'j'},
        {"enable-gc",     no_argument,       0, 'g'},
        {"enable-udp",    no_argument,       0, 'U'}, // 修改为 -U
        {"udp-port",      required_argument, 0, 258},
        {"trigger-gc",    no_argument,       0, 256},
        {"stop-gc",       no_argument,       0, 257},
        {0, 0, 0, 0}
    };

    int opt;
    char *blacklist1_file = NULL, *blacklist2_file = NULL, *whitelist_file = NULL;
    int seconds = 0, days = 0, debug_level = 1;
    int enable_gc = 1; // GC 功能现在默认启用，但需要 F2FS 文件系统
    int enable_udp_server = 0; // UDP 服务器默认禁用
    int trigger_gc_flag = 0, stop_gc_flag = 0;
    int udp_port = DEFAULT_UDP_PORT;
    long long json_log_max_size = 64 * 1024;

    // 解析命令行参数
    while ((opt = getopt_long(argc, argv, "1:2:w:D:s:d:j:gU", long_options, NULL)) != -1) { // 修改为 U
        switch (opt) {
            case '1': blacklist1_file = optarg; break;
            case '2': blacklist2_file = optarg; break;
            case 'w': whitelist_file = optarg; break;
            case 'D': days = atoi(optarg); break;
            case 's': seconds = atoi(optarg); break;
            case 'd': debug_level = atoi(optarg); break;
            case 'j': json_log_max_size = atoll(optarg) * 1024; break;
            case 'g': enable_gc = 1; break; // 显式启用 GC
            case 'U': enable_udp_server = 1; break; // 启用 UDP 服务器
            case 256: trigger_gc_flag = 1; break;
            case 257: stop_gc_flag = 1; break;
            case 258: udp_port = atoi(optarg); break;
            case '?': print_usage(argv[0]); return EXIT_FAILURE;
            default: abort();
        }
    }

    // 初始化日志系统
    if (log_init(LOG_FILE, debug_level) != 0) {
        return EXIT_FAILURE;
    }

    // 处理一次性 GC 触发命令
    if (trigger_gc_flag) {
        handle_gc_trigger();
        log_cleanup();
        return EXIT_SUCCESS;
    }
    
    // 处理手动停止 GC 命令
    if (stop_gc_flag) {
        log_message(1, "执行手动停止 GC 命令。");
        stop_current_gc(); // 调用 gc_trim 模块的停止函数
        log_message(1, "停止 GC 命令已发送。");
        log_cleanup();
        return EXIT_SUCCESS;
    }

    // 检查必要参数
    if (!blacklist1_file || !whitelist_file) {
        print_usage(argv[0]);
        log_message(0, "错误: 必须同时提供黑名单1和白名单文件路径。");
        log_cleanup();
        return EXIT_FAILURE;
    }
    if (blacklist2_file && days <= 0) {
        fprintf(stderr, "错误: 使用黑名单2时，必须设置大于0的过期天数。\n");
        log_message(0, "错误: 使用黑名单2时，必须设置大于0的过期天数。");
        log_cleanup();
        return EXIT_FAILURE;
    }

    log_message(1, "\n==================================================");
    log_message(1, "程序启动 (PID: %d)", getpid());
    log_message(1, "配置: 黑名单1='%s', 黑名单2='%s', 白名单='%s', 过期天数=%d, 循环间隔=%d秒, GC/Trim: %s, UDP服务器: %s (端口: %d)",
                blacklist1_file, blacklist2_file ? blacklist2_file : "(无)", whitelist_file, days, seconds,
                enable_gc ? "已启用" : "已禁用", enable_udp_server ? "已启用" : "已禁用", udp_port);

    // 初始化统计模块
    if (stats_init(JSON_STATS_FILE, json_log_max_size, APP_LOG_FILE) != 0) {
        log_cleanup();
        return EXIT_FAILURE;
    }
    // 初始化配置管理器
    config_init();

    // 初始化 GC/Trim 模块 (获取 F2FS sysfs 路径)
    if (gc_trim_init() != 0) {
        log_message(0, "GC/Trim 模块初始化失败，相关功能将不可用。");
        enable_gc = 0; // 如果初始化失败，则禁用 GC 功能
    }

    // 启动 UDP 服务器 (如果启用)
    if (enable_udp_server) {
        if (udp_server_start(udp_port) != 0) {
            log_message(0, "无法启动 UDP 服务器，将不提供远程控制功能。");
            enable_udp_server = 0; // 如果启动失败，则禁用该功能
        }
    }

    // 将 manual_clean_request 声明移到 do-while 循环外部
    int manual_clean_request = 0; // <-- 修正：声明移到这里

    // 主循环
    do {
        if (enable_udp_server) {
            manual_clean_request = udp_check_and_clear_clean_request();
            if (manual_clean_request) {
                log_message(1, "收到来自 UDP 的立即清理请求。");
            }
        }

        time_t loop_start_time = time(NULL);
        char time_str[30];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&loop_start_time));
        log_message(1, "\n---【主循环开始】 时间: %s ---", time_str);
        log_check_and_rotate(MAX_LOG_SIZE); // 检查并轮转主日志文件

        stats_reset_cycle(); // 重置本轮清理的统计数据

        // 加载或重新加载配置文件
        const ConfigData* config = config_load(blacklist1_file, blacklist2_file, whitelist_file);
        if (!config || config->wl_count == 0) { // 确保白名单有效
            log_message(0, "加载配置文件失败或白名单为空，无法继续。跳过本轮处理。");
        } else {
            // 处理黑名单1 (无过期检查)
            if (config->blacklist1 && config->bl1_count > 0) {
                cleaner_process_blacklist1(config);
            } else {
                log_message(2, "跳过黑名单1处理 (无有效规则加载)。");
            }
            // 处理黑名单2 (有过期检查)
            if (blacklist2_file && config->blacklist2 && config->bl2_count > 0) {
                cleaner_process_blacklist2(config, days);
            } else if (blacklist2_file) {
                log_message(2, "跳过黑名单2处理 (无有效规则加载)。");
            }
        }

        time_t loop_end_time = time(NULL);
        double loop_duration = difftime(loop_end_time, loop_start_time);
        
        // 获取 GC 统计数据 (即使 GC 线程不常驻，get_gc_trim_stats 也能获取上次 GC 的结果)
        GCTrimStats gc_stats = get_gc_trim_stats();
        stats_log_summary(loop_duration, gc_stats, enable_gc); // 记录统计数据

        log_message(1, "---【主循环结束】 耗时: %.2f 秒 ---", loop_duration);
        
        // 根据循环间隔和手动清理请求决定是否休眠
        if (seconds > 0 && !manual_clean_request) {
            log_message(1, "等待 %d 秒进入下一主循环...", seconds);
            sleep(seconds);
        } else if (seconds == 0) {
            log_message(1, "单次执行完成，程序退出。");
            break; // 单次运行模式，执行完就退出
        }
        // 如果是手动清理请求，且 seconds > 0，则不休眠，立即进入下一轮循环
        // 如果 seconds == 0，且有手动清理请求，则在处理完后退出 (因为上面的 break)
    } while (seconds > 0 || manual_clean_request); // 循环条件：定时循环 或 有手动清理请求

    log_message(1, "程序正在关闭，释放资源...");
    if (enable_udp_server) {
        udp_server_stop(); // 停止 UDP 服务器
    }
    // GC 线程是动态创建和销毁的，不需要在这里显式停止
    config_free();   // 清理配置管理器资源
    stats_cleanup(); // 清理统计管理器资源
    log_cleanup();   // 清理日志系统资源

    return EXIT_SUCCESS;
}