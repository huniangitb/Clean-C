#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fnmatch.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>

/* 全局变量定义 */
int debug_level = 2;        /* 调试级别: 0=无日志, 1=基本日志, 2=详细日志 */
int total_files_deleted = 0; /* 已删除文件计数器 */
int total_dirs_deleted = 0;  /* 已删除目录计数器 */
FILE *log_file;             /* 日志文件句柄 */

/* 特殊规则结构体定义 */
typedef struct {
    char path[PATH_MAX];     /* 路径 */
    char **patterns;         /* 匹配模式数组 */
    int pattern_count;       /* 模式数量 */
} SpecialRule;

/**
 * 统一删除操作函数
 * @param path: 要删除的项目路径
 * @param is_dir: 是否为目录(1=目录, 0=文件)
 */
static void delete_item(const char *path, int is_dir) {
    if (!path) return;  /* 添加空指针检查 */
    
    if (is_dir) {
        if (rmdir(path) == 0) {
            total_dirs_deleted++;
            if (debug_level >= 2) fprintf(log_file, "删除目录: %s\n", path);
        } else if (debug_level >= 2) {
            fprintf(log_file, "删除目录失败: %s, 错误: %s\n", path, strerror(errno));
        }
    } else {
        struct stat statbuf;
        int is_link = (lstat(path, &statbuf) == 0 && S_ISLNK(statbuf.st_mode));

        if (remove(path) == 0) {
            total_files_deleted++;
            if (debug_level >= 2) {
                fprintf(log_file, "删除%s: %s\n", 
                       is_link ? "符号链接" : "文件", path);
            }
        } else if (debug_level >= 2) {
            fprintf(log_file, "删除%s失败: %s, 错误: %s\n",
                   is_link ? "符号链接" : "文件", path, strerror(errno));
        }
    }
}

/**
 * 解析特殊规则函数
 * @param rule_str: 规则字符串
 * @param rule: 特殊规则结构体指针
 * @return: 1=成功, 0=失败
 */
static int parse_special_rule(const char *rule_str, SpecialRule *rule) {
    if (!rule_str || !rule) return 0;  /* 参数验证 */
    
    char *pattern_start = strchr(rule_str, '[');
    if (!pattern_start || !strchr(pattern_start, ']')) return 0;

    size_t path_len = pattern_start - rule_str;
    if (path_len >= PATH_MAX-1) return 0;

    strncpy(rule->path, rule_str, path_len);
    rule->path[path_len] = '\0';

    char *pattern_end = strchr(pattern_start, ']');
    size_t pattern_len = pattern_end - pattern_start - 1;
    char *patterns = malloc(pattern_len + 1);  /* 动态分配内存 */
    if (!patterns) return 0;

    strncpy(patterns, pattern_start + 1, pattern_len);
    patterns[pattern_len] = '\0';

    int capacity = 5;
    rule->pattern_count = 0;
    rule->patterns = malloc(sizeof(char*) * capacity);
    if (!rule->patterns) {
        free(patterns);
        return 0;
    }

    char *token = strtok(patterns, "|");
    while (token) {
        if (rule->pattern_count >= capacity) {
            capacity *= 2;
            char **new = realloc(rule->patterns, sizeof(char*) * capacity);
            if (!new) {
                free(patterns);
                for (int i = 0; i < rule->pattern_count; i++) {
                    free(rule->patterns[i]);
                }
                free(rule->patterns);
                return 0;
            }
            rule->patterns = new;
        }
        rule->patterns[rule->pattern_count] = strdup(token);
        if (!rule->patterns[rule->pattern_count]) {
            free(patterns);
            for (int i = 0; i < rule->pattern_count; i++) {
                free(rule->patterns[i]);
            }
            free(rule->patterns);
            return 0;
        }
        rule->pattern_count++;
        token = strtok(NULL, "|");
    }
    
    free(patterns);
    return 1;
}

/**
 * 文件名模式匹配检查
 * @param filename: 文件名
 * @param patterns: 模式数组
 * @param count: 模式数量
 * @return: 1=匹配, 0=不匹配
 */
static int filename_matches(const char *filename, const char **patterns, int count) {
    if (!filename) return 0;  /* 参数验证 */
    if (count == 0) return 1;
    if (!patterns) return 0;

    for (int i = 0; i < count; i++) {
        if (patterns[i] && fnmatch(patterns[i], filename, 0) == 0) 
            return 1;
    }
    return 0;
}
/**
 * 检查路径是否在白名单中
 * @param path: 要检查的路径
 * @param whitelist: 白名单数组
 * @param whitelist_count: 白名单条目数量
 * @return: 1=在白名单中, 0=不在白名单中
 */
int is_in_whitelist(const char *path, char **whitelist, int whitelist_count) {
    if (!path || !whitelist || whitelist_count <= 0) return 0;
    
    for (int i = 0; i < whitelist_count; i++) {
        if (!whitelist[i]) continue;  /* 跳过无效条目 */
        
        if (strchr(whitelist[i], '[') && strchr(whitelist[i], ']')) {
            /* 处理带模式的白名单规则 */
            char *pattern_start = strchr(whitelist[i], '[');
            size_t path_len = pattern_start - whitelist[i];
            if (path_len >= PATH_MAX) continue;

            char white_path[PATH_MAX];
            strncpy(white_path, whitelist[i], path_len);
            white_path[path_len] = '\0';

            char *pattern_end = strchr(pattern_start, ']');
            if (!pattern_end) continue;

            size_t pattern_len = pattern_end - pattern_start - 1;
            char *white_patterns = malloc(pattern_len + 1);
            if (!white_patterns) continue;

            strncpy(white_patterns, pattern_start + 1, pattern_len);
            white_patterns[pattern_len] = '\0';

            if (fnmatch(white_path, path, FNM_PATHNAME) == 0) {
                const char *filename = strrchr(path, '/');
                filename = filename ? filename + 1 : path;

                char *token = strtok(white_patterns, "|");
                while (token) {
                    if (fnmatch(token, filename, 0) == 0) {
                        free(white_patterns);
                        return 1;
                    }
                    token = strtok(NULL, "|");
                }
            }
            free(white_patterns);
        } else {
            /* 处理白名单规则 */
            if (fnmatch(whitelist[i], path, FNM_PATHNAME) == 0) return 1;
        }
    }
    return 0;
}

/**
 * 从文件读取内容到数组
 * @param filename: 文件名
 * @param array: 指向字符串数组指针的指针
 * @return: 读取的行数
 */
int read_file_to_array(const char *filename, char ***array) {
    if (!filename || !array) return 0;

    FILE *file = fopen(filename, "r");
    if (!file) {
        if (debug_level >= 2) 
            fprintf(log_file, "无法打开文件: %s, 错误: %s\n", filename, strerror(errno));
        return 0;
    }

    char *line = NULL;
    size_t line_len = 0;
    ssize_t read;

    int count = 0;
    int capacity = 10;
    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        fclose(file);
        return 0;
    }

    while ((read = getline(&line, &line_len, file)) != -1) {
        if (read > 0 && line[read-1] == '\n') line[read-1] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;  /* 跳过注释和空行 */

        /* 动态扩展数组容量 */
        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                for (int j = 0; j < count; j++) free(temp_array[j]);
                free(temp_array);
                free(line);
                fclose(file);
                return 0;
            }
            temp_array = new_array;
        }

        /* 复制行内容 */
        temp_array[count] = strdup(line);
        if (!temp_array[count]) {
            for (int j = 0; j < count; j++) free(temp_array[j]);
            free(temp_array);
            free(line);
            fclose(file);
            return 0;
        }
        count++;
    }

    free(line);
    fclose(file);
    *array = temp_array;
    return count;
}

/**
 * 检查文件是否过期
 * @param path: 文件路径
 * @param days: 过期天数
 * @return: 1=已过期, 0=未过期
 */
int is_expired(const char *path, int days) {
    if (!path || days < 0) return 0;

    struct stat statbuf;
    if (lstat(path, &statbuf) == 0) {
        time_t current_time = time(NULL);
        if (current_time == (time_t)-1) return 0;

        double diff_time = difftime(current_time, statbuf.st_mtime);
        return (diff_time > (double)days * 24 * 3600);
    }
    return 0;
}

/**
 * 递归删除目录内容
 * @param path: 目录路径
 * @param whitelist: 白名单数组
 * @param wl_count: 白名单条目数量
 * @param patterns: 匹配模式数组
 * @param p_count: 模式数量
 * @param check_expiry: 是否检查过期
 * @param days: 过期天数
 */
void delete_directory_recursive(const char *path, char **whitelist, int wl_count,
     const char **patterns, int p_count, 
    int check_expiry, int days) {
    if (!path) return;

    DIR *dir = opendir(path);
    if (!dir) {
        if (debug_level >= 2) 
            fprintf(log_file, "无法打开目录: %s, 错误: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
            continue;

        char full_path[PATH_MAX];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >= PATH_MAX) {
            if (debug_level >= 2) 
                fprintf(log_file, "路径过长: %s/%s\n", path, entry->d_name);
            continue;
        }

        if (is_in_whitelist(full_path, whitelist, wl_count)) continue;

        struct stat statbuf;
        if (lstat(full_path, &statbuf) != 0) {
            if (debug_level >= 2) 
                fprintf(log_file, "无法获取文件信息: %s, 错误: %s\n", full_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            delete_directory_recursive(full_path, whitelist, wl_count, 
                                    patterns, p_count, check_expiry, days);
            delete_item(full_path, 1);
        } else {
            const char *filename = strrchr(full_path, '/');
            filename = filename ? filename + 1 : full_path;

            int should_delete = filename_matches(filename, patterns, p_count) &&
                              (!check_expiry || is_expired(full_path, days));

            if (should_delete) delete_item(full_path, 0);
        }
    }
    closedir(dir);
}
/**
 * 统一黑名单处理函数
 * @param blacklist: 黑名单数组
 * @param count: 黑名单条目数量
 * @param whitelist: 白名单数组
 * @param wl_count: 白名单条目数量
 * @param check_expiry: 是否检查过期
 * @param days: 过期天数
 */
static void process_blacklist_generic(
    char **blacklist, int count,
    char **whitelist, int wl_count,
    int check_expiry, int days
) {
    if (!blacklist || count <= 0) return;

    SpecialRule rule;
    for (int i = 0; i < count; i++) {
        if (!blacklist[i]) continue;  /* 跳过无效条目 */

        memset(&rule, 0, sizeof(rule));
        const char *target_path = blacklist[i];
        const char **patterns = NULL;
        int p_count = 0;

        /* 处理特殊规则 */
        if (strchr(blacklist[i], '[') && parse_special_rule(blacklist[i], &rule)) {
            target_path = rule.path;
            patterns = (const char **)rule.patterns;
            p_count = rule.pattern_count;
        }

        /* 检查白名单 */
        if (!is_in_whitelist(target_path, whitelist, wl_count)) {
            struct stat statbuf;
            if (lstat(target_path, &statbuf) != 0) {
                if (debug_level >= 2) 
                    fprintf(log_file, "路径不存在，跳过: %s\n", target_path);
                goto cleanup;
            }

            /* 处理目录和文件 */
            if (S_ISDIR(statbuf.st_mode)) {
                delete_directory_recursive(target_path, whitelist, wl_count, 
                                        patterns, p_count, check_expiry, days);
                delete_item(target_path, 1);
            } else {
                const char *filename = strrchr(target_path, '/');
                filename = filename ? filename + 1 : target_path;

                if ((!check_expiry || is_expired(target_path, days)) &&
                    filename_matches(filename, patterns, p_count)) {
                    delete_item(target_path, 0);
                }
            }
        }

cleanup:
        /* 清理特殊规则资源 */
        for (int j = 0; j < rule.pattern_count; j++) {
            free(rule.patterns[j]);
        }
        free(rule.patterns);
    }
}

/**
 * 处理不检查过期时间的黑名单
 * @param blacklist: 黑名单数组
 * @param count: 黑名单条目数量
 * @param whitelist: 白名单数组
 * @param wl_count: 白名单条目数量
 */
void process_blacklist1(char **blacklist, int count, char **whitelist, int wl_count) {
    process_blacklist_generic(blacklist, count, whitelist, wl_count, 0, 0);
}

/**
 * 处理需要检查过期时间的黑名单
 * @param blacklist: 黑名单数组
 * @param count: 黑名单条目数量
 * @param whitelist: 白名单数组
 * @param wl_count: 白名单条目数量
 * @param days: 过期天数
 */
void process_blacklist2(char **blacklist, int count, char **whitelist, int wl_count, int days) {
    process_blacklist_generic(blacklist, count, whitelist, wl_count, 1, days);
}

/**
 * 释放字符串数组
 * @param array: 要释放的数组
 * @param count: 数组元素数量
 */
void free_array(char **array, int count) {
    if (!array) return;
    for (int i = 0; i < count; i++) {
        free(array[i]);
    }
    free(array);
}

/**
 * 主函数
 * @param argc: 命令行参数数量
 * @param argv: 命令行参数数组
 * @return: 执行状态码
 */
int main(int argc, char *argv[]) {
    /* 定义长选项 */
    static struct option long_options[] = {
        {"seconds", required_argument, 0, 's'},
        {"debug", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int seconds = 0;

    /* 解析命令行参数 */
    while ((opt = getopt_long(argc, argv, "s:d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                seconds = atoi(optarg);
                if (seconds < 0) {
                    fprintf(stderr, "无效的时间间隔\n");
                    return EXIT_FAILURE;
                }
                break;
            case 'd':
                debug_level = (optarg[0] == '0') ? 0 : 
                            (optarg[0] == '1') ? 1 : 2;
                break;
            case 'h':
                printf("用法: %s <blacklist1> <blacklist2> <whitelist> <天数> "
                       "[-s seconds] [-d debug_level]\n", argv[0]);
                return EXIT_SUCCESS;
            default:
                return EXIT_FAILURE;
        }
    }

    /* 检查必需参数 */
    if (argc - optind < 4) {
        fprintf(stderr, "参数不足\n");
        return EXIT_FAILURE;
    }

    char *blacklist1_file = argv[optind];
    char *blacklist2_file = argv[optind+1];
    char *whitelist_file = argv[optind+2];
    int days = atoi(argv[optind+3]);

    if (days < 0) {
        fprintf(stderr, "无效的天数\n");
        return EXIT_FAILURE;
    }

    /* 主循环 */
    do {
        /* 打开日志文件 */
        log_file = fopen("run.log", "a");
        if (!log_file) {
            perror("无法打开日志文件");
            return EXIT_FAILURE;
        }

        /* 记录开始时间 */
        time_t start_time = time(NULL);
        char time_str[100];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", 
                localtime(&start_time));
        if (debug_level >= 2) 
            fprintf(log_file, "程序启动时间: %s\n", time_str);

        /* 读取配置文件 */
        char **blacklist1 = NULL, **blacklist2 = NULL, **whitelist = NULL;
        int bl1_count = read_file_to_array(blacklist1_file, &blacklist1);
        int bl2_count = read_file_to_array(blacklist2_file, &blacklist2);
        int wl_count = read_file_to_array(whitelist_file, &whitelist);

        /* 处理黑名单 */
        process_blacklist1(blacklist1, bl1_count, whitelist, wl_count);
        process_blacklist2(blacklist2, bl2_count, whitelist, wl_count, days);

        /* 清理资源 */
        free_array(blacklist1, bl1_count);
        free_array(blacklist2, bl2_count);
        free_array(whitelist, wl_count);

        /* 记录结束时间和统计信息 */
        time_t end_time = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", 
                localtime(&end_time));
        if (debug_level >= 1) {
            fprintf(log_file, "%s 已删除文件数: %d\n", 
                    time_str, total_files_deleted);
            fprintf(log_file, "%s 已删除目录数: %d\n", 
                    time_str, total_dirs_deleted);
        }

        /* 重置计数器 */
        total_files_deleted = total_dirs_deleted = 0;
        fclose(log_file);

        /* 等待下一次执行 */
        if (seconds > 0) sleep(seconds);
    } while (seconds > 0);

    return EXIT_SUCCESS;
}