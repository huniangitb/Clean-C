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
#include <pwd.h>
#include <grp.h>
#include <stdarg.h>

/* 全局变量定义 */
int debug_level = 1;        /* 调试级别: 0=无日志, 1=基本日志, 2=详细日志 */
int total_files_deleted = 0; /* 已删除文件计数器 */
int total_dirs_deleted = 0;  /* 已删除目录计数器 */
FILE *log_file;             /* 日志文件句柄 */
char program_name[PATH_MAX]; /* 程序名称 */
uid_t target_uid = -1;       /* 目标用户ID */
gid_t target_gid = -1;       /* 目标组ID */

#define MAX_LOG_SIZE (256 * 1024) /* 最大日志文件大小: 256KB */

/* 特殊规则结构体定义 */
typedef struct {
    char path[PATH_MAX];     /* 路径 */
    char **patterns;         /* 匹配模式数组 */
    int pattern_count;       /* 模式数量 */
} SpecialRule;

/**
 * 获取文件大小
 * @param filename: 文件名
 * @return: 文件大小，-1 表示出错
 */
long long get_file_size(const char *filename) {
    struct stat statbuf;
    if (stat(filename, &statbuf) == 0) {
        return statbuf.st_size;
    }
    return -1;
}

/**
 * 日志文件滚动
 */
void rotate_log_file() {
    fclose(log_file);
    if (rename("run.log", "run.log.old") != 0) {
        fprintf(stderr, "日志文件滚动失败: %s\n", strerror(errno));
    } else if (debug_level >= 1) {
        fprintf(stderr, "日志文件已滚动到 run.log.old\n");
    }
    log_file = fopen("run.log", "a");
    if (!log_file) {
        perror("无法打开新的日志文件");
        exit(EXIT_FAILURE);
    }
    if (debug_level >= 1) {
        fprintf(log_file, "新日志文件已打开，继续运行。\n");
        fflush(log_file);
    }
}

/**
 * 检查并滚动日志文件
 */
void check_and_rotate_log() {
    if (get_file_size("run.log") > MAX_LOG_SIZE) {
        rotate_log_file();
    }
}

/**
 * 统一日志输出函数
 * @param level: 日志级别 (只有当全局 debug_level >= level 时才输出)
 * @param format: 格式化字符串，类似于 printf
 * @param ...: 可变参数
 */
void log_message(int level, const char *format, ...) {
    if (debug_level >= level) {
        check_and_rotate_log();
        va_list args;
        va_start(args, format);
        vfprintf(log_file, format, args);
        va_end(args);
        fflush(log_file);
    }
}


/**
 * 统一删除操作函数
 * @param path: 要删除的项目路径
 * @param is_dir: 是否为目录(1=目录, 0=文件)
 */
static void delete_item(const char *path, int is_dir) {
    if (!path) return;

    if (is_dir) {
        if (rmdir(path) == 0) {
            total_dirs_deleted++;
            log_message(2, "删除目录: %s\n", path);
        } else {
            log_message(1, "删除目录失败: %s, 错误: %s\n", path, strerror(errno));
        }
    } else {
        struct stat statbuf;
        int is_link = (lstat(path, &statbuf) == 0 && S_ISLNK(statbuf.st_mode));
        if (remove(path) == 0) {
            total_files_deleted++;
            log_message(2, "删除%s: %s\n", is_link ? "符号链接" : "文件", path);
        } else {
            log_message(1, "删除%s失败: %s, 错误: %s\n", is_link ? "符号链接" : "文件", path, strerror(errno));
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
    if (!rule_str || !rule) return 0;

    char *pattern_start = strchr(rule_str, '[');
    if (!pattern_start || !strchr(pattern_start, ']')) return 0;

    size_t path_len = pattern_start - rule_str;
    if (path_len >= PATH_MAX) return 0;

    strncpy(rule->path, rule_str, path_len);
    rule->path[path_len] = '\0';

    char *pattern_end = strchr(pattern_start, ']');
    if (!pattern_end) return 0;

    size_t pattern_len = pattern_end - pattern_start - 1;
    char *patterns_str = malloc(pattern_len + 1);
    if (!patterns_str) {
        log_message(1, "[错误] 内存分配失败: patterns_str\n");
        return 0;
    }
    strncpy(patterns_str, pattern_start + 1, pattern_len);
    patterns_str[pattern_len] = '\0';

    const int initial_capacity = 5;
    rule->pattern_count = 0;
    rule->patterns = malloc(sizeof(char*) * initial_capacity);
    if (!rule->patterns) {
        log_message(1, "[错误] 内存分配失败: rule->patterns\n");
        free(patterns_str);
        return 0;
    }
    int capacity = initial_capacity;

    char *token = strtok(patterns_str, "|");
    while (token) {
        if (rule->pattern_count >= capacity) {
            capacity *= 2;
            char **new_patterns = realloc(rule->patterns, sizeof(char*) * capacity);
            if (!new_patterns) {
                log_message(1, "[错误] 内存重分配失败: new_patterns\n");
                free(patterns_str);
                for (int i = 0; i < rule->pattern_count; i++) free(rule->patterns[i]);
                free(rule->patterns);
                return 0;
            }
            rule->patterns = new_patterns;
        }
        rule->patterns[rule->pattern_count] = strdup(token);
        if (!rule->patterns[rule->pattern_count]) {
            log_message(1, "[错误] 内存分配失败: rule->patterns[%d]\n", rule->pattern_count);
            free(patterns_str);
            for (int i = 0; i < rule->pattern_count; i++) free(rule->patterns[i]);
            free(rule->patterns);
            return 0;
        }
        rule->pattern_count++;
        token = strtok(NULL, "|");
    }

    free(patterns_str);
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
    if (!filename) return 0;
    if (count == 0) return 1;
    if (!patterns) return 0;

    for (int i = 0; i < count; i++) {
        if (patterns[i] && fnmatch(patterns[i], filename, 0) == 0) return 1;
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
        if (!whitelist[i]) continue;
        if (fnmatch(whitelist[i], path, FNM_PATHNAME) == 0) return 1;
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
        log_message(1, "无法打开文件: %s, 错误: %s\n", filename, strerror(errno));
        return 0;
    }

    char *line = NULL;
    size_t line_len = 0;
    ssize_t read;
    int count = 0;
    int capacity = 10;
    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        log_message(1, "[错误] 内存分配失败: temp_array\n");
        fclose(file);
        return 0;
    }

    while ((read = getline(&line, &line_len, file)) != -1) {
        if (read > 0 && line[read-1] == '\n') line[read-1] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                log_message(1, "[错误] 内存重分配失败: new_array\n");
                for (int j = 0; j < count; j++) free(temp_array[j]);
                free(temp_array);
                free(line);
                fclose(file);
                return 0;
            }
            temp_array = new_array;
        }
        temp_array[count] = strdup(line);
        if (!temp_array[count]) {
            log_message(1, "[错误] 内存分配失败: temp_array[%d]\n", count);
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
    log_message(2, "从文件 %s 读取了 %d 行规则\n", filename, count);
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
        if (current_time == (time_t)-1) {
            log_message(1, "[警告] 获取当前时间失败\n");
            return 0;
        }
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
 * @param  skip_root: 是否跳过根目录
 */
void delete_directory_recursive(const char *path, char **whitelist, int wl_count,
    const char **patterns, int p_count, int check_expiry, int days, int skip_root) {
    if (!path) return;

    if (!skip_root && is_in_whitelist(path, whitelist, wl_count)) {
        log_message(2, "目录在白名单中，跳过: %s\n", path);
        return;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        log_message(1, "无法打开目录: %s, 错误: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full_path[PATH_MAX];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >= PATH_MAX) {
            log_message(1, "路径过长: %s/%s\n", path, entry->d_name);
            continue;
        }

        if (is_in_whitelist(full_path, whitelist, wl_count)) {
            log_message(2, "项在白名单中，跳过: %s\n", full_path);
            continue;
        }

        struct stat statbuf;
        if (lstat(full_path, &statbuf) != 0) {
            log_message(1, "无法获取文件信息: %s, 错误: %s\n", full_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            delete_directory_recursive(full_path, whitelist, wl_count,
                                        patterns, p_count, check_expiry, days, 0);
        } else {
            const char *filename = strrchr(full_path, '/');
            filename = filename ? filename + 1 : full_path;
            if (filename_matches(filename, patterns, p_count) && (!check_expiry || is_expired(full_path, days))) {
                delete_item(full_path, 0);
            }
        }
    }
    closedir(dir);

    if (!skip_root) {
        dir = opendir(path);
        if (dir) {
            int is_empty = 1;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                    is_empty = 0;
                    break;
                }
            }
            closedir(dir);
            if (is_empty) {
                delete_item(path, 1);
            }
        }
    }
}

/**
 * 处理黑名单
 * @param blacklist: 黑名单数组
 * @param count: 黑名单条目数量
 * @param whitelist: 白名单数组
 * @param wl_count: 白名单条目数量
 * @param check_expiry: 是否检查过期
 * @param days: 过期天数
 */
static void process_blacklist(char **blacklist, int count, char **whitelist, int wl_count, int check_expiry, int days) {
    if (!blacklist || count <= 0) return;

    SpecialRule rule;
    for (int i = 0; i < count; i++) {
        if (!blacklist[i]) continue;

        memset(&rule, 0, sizeof(rule));
        const char *target_path = blacklist[i];
        const char **patterns = NULL;
        int p_count = 0;

        if (strchr(blacklist[i], '[') && parse_special_rule(blacklist[i], &rule)) {
            target_path = rule.path;
            patterns = (const char **)rule.patterns;
            p_count = rule.pattern_count;
        }

        if (!is_in_whitelist(target_path, whitelist, wl_count)) {
            struct stat statbuf;
            if (lstat(target_path, &statbuf) != 0) {
                log_message(2, "路径不存在，跳过: %s\n", target_path);
                goto cleanup;
            }

            if (S_ISDIR(statbuf.st_mode)) {
                if (patterns && p_count > 0 && strchr(blacklist[i], '[')) {
                    DIR *dir_to_process = opendir(target_path);
                    if (dir_to_process) {
                        struct dirent *dir_entry;
                        while ((dir_entry = readdir(dir_to_process)) != NULL) {
                            if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0) continue;
                            char item_path[PATH_MAX];
                            snprintf(item_path, sizeof(item_path), "%s/%s", target_path, dir_entry->d_name);

                            struct stat item_statbuf;
                            if (lstat(item_path, &item_statbuf) == 0) {
                                if (!S_ISDIR(item_statbuf.st_mode)) {
                                    const char *filename = dir_entry->d_name;
                                    if ((!check_expiry || is_expired(item_path, days)) && filename_matches(filename, patterns, p_count)) {
                                        delete_item(item_path, 0);
                                    }
                                }
                            } else {
                                log_message(1, "无法获取文件信息: %s, 错误: %s\n", item_path, strerror(errno));
                            }
                        }
                        closedir(dir_to_process);

                        dir_to_process = opendir(target_path);
                        if (dir_to_process) {
                            int is_empty_after_delete = 1;
                            struct dirent *check_entry;
                            while ((check_entry = readdir(dir_to_process)) != NULL) {
                                if (strcmp(check_entry->d_name, ".") != 0 && strcmp(check_entry->d_name, "..") != 0) {
                                    is_empty_after_delete = 0;
                                    break;
                                }
                            }
                            closedir(dir_to_process);
                            if (is_empty_after_delete) {
                                log_message(2, "删除空目录 (规则带通配符): %s\n", target_path);
                                delete_item(target_path, 1);
                            } else {
                                log_message(2, "保留目录 (规则带通配符，非空): %s\n", target_path);
                            }
                        }
                    } else {
                        log_message(1, "无法打开目录进行处理 (通配符规则): %s, 错误: %s\n", target_path, strerror(errno));
                    }
                } else {
                    delete_directory_recursive(target_path, whitelist, wl_count,
                                            patterns, p_count, check_expiry, days, 1);
                    log_message(2, "删除空目录 (规则不带通配符): %s\n", target_path);
                    delete_item(target_path, 1);
                }
            } else {
                const char *filename = strrchr(target_path, '/');
                filename = filename ? filename + 1 : target_path;
                if ((!check_expiry || is_expired(target_path, days)) && filename_matches(filename, patterns, p_count)) {
                    delete_item(target_path, 0);
                }
            }
        } else {
            log_message(2, "黑名单路径在白名单中，跳过: %s\n", target_path);
        }

    cleanup:
        for (int j = 0; j < rule.pattern_count; j++) free(rule.patterns[j]);
        free(rule.patterns);
    }
}


/**
 * 释放字符串数组
 * @param array: 要释放的数组
 * @param count: 数组元素数量
 */
void free_array(char **array, int count) {
    if (!array) return;
    for (int i = 0; i < count; i++) free(array[i]);
    free(array);
}

/**
 * 打印帮助信息
 */
void print_help(const char *program_name) {
    printf("用法:\n");
    printf("  %s [选项]\n", program_name);
    printf("\n选项:\n");
    printf("  -1 <blacklist1>, --blacklist1=<blacklist1>  不检查过期时间的黑名单文件路径。\n");
    printf("  -2 <blacklist2>, --blacklist2=<blacklist2>  检查过期时间的黑名单文件路径。\n");
    printf("  -w <whitelist>, --whitelist=<whitelist>    白名单文件路径，每行一条路径规则。\n");
    printf("  -D <days>, --days=<days>             文件过期天数 (与 -2 选项一起使用，必须为非负整数)。\n");
    printf("  -s <seconds>, --seconds=<seconds>  设置程序循环执行的时间间隔，单位为秒 (必须为非负整数，0 表示单次执行)。\n");
    printf("  -d <debug_level>, --debug=<debug_level>  设置调试级别 (0=无日志, 1=基本日志, 2=详细日志)。\n");
    printf("  -u <uid>, --uid=<uid>  以指定用户ID运行 (可以是用户名或数字UID)。\n");
    printf("  -g <gid>, --gid=<gid>  以指定组ID运行 (可以是组名或数字GID)。\n");
    printf("  -h, --help       显示帮助信息。\n");
    printf("\n注意:\n");
    printf("  - 黑名单和白名单文件每行一条规则，# 开头的行和空行会被忽略。\n");
    printf("  - 规则可以使用通配符，具体参考 fnmatch 函数。\n");
    printf("  - 黑名单特殊规则格式: <路径>[<模式1>|<模式2>|...], 例如: /tmp/cache/[*.tmp|*.log]\n");
    printf("  - 白名单仅支持简单的路径规则，不支持模式匹配。\n");
    printf("\n示例:\n");
    printf("  %s -1 blacklist1.txt -w whitelist.txt -s 60 -d 1\n", program_name);
    printf("  %s -1 blacklist1.txt -2 blacklist2.txt -w whitelist.txt -D 30\n", program_name);
}


/**
 * 主函数
 * @param argc: 命令行参数数量
 * @param argv: 命令行参数数组
 * @return: 执行状态码
 */
int main(int argc, char *argv[]) {
    strncpy(program_name, argv[0], sizeof(program_name) - 1);
    program_name[sizeof(program_name) - 1] = '\0';

    static struct option long_options[] = {
        {"blacklist1", required_argument, 0, '1'},
        {"blacklist2", required_argument, 0, '2'},
        {"whitelist", required_argument, 0, 'w'},
        {"days", required_argument, 0, 'D'},
        {"seconds", required_argument, 0, 's'},
        {"debug", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"uid", required_argument, 0, 'u'},
        {"gid", required_argument, 0, 'g'},
        {0, 0, 0, 0}
    };

    int opt;
    int seconds = 0;
    char *uid_str = NULL;
    char *gid_str = NULL;
    char *blacklist1_file = NULL;
    char *blacklist2_file = NULL;
    char *whitelist_file = NULL;
    int days = 0;


    while ((opt = getopt_long(argc, argv, "1:2:w:D:s:d:hu:g:", long_options, NULL)) != -1) {
        switch (opt) {
            case '1': blacklist1_file = optarg; break;
            case '2': blacklist2_file = optarg; break;
            case 'w': whitelist_file = optarg; break;
            case 'D':
                days = atoi(optarg);
                if (days < 0) {
                    fprintf(stderr, "%s: 错误: 无效的天数 '%s', 必须为非负整数。\n", program_name, optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 's':
                seconds = atoi(optarg);
                if (seconds < 0) {
                    fprintf(stderr, "%s: 错误: 无效的时间间隔 '%s', 必须为非负整数。\n", program_name, optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'd':
                debug_level = atoi(optarg);
                if (debug_level < 0 || debug_level > 2) {
                    fprintf(stderr, "%s: 错误: 无效的调试级别 '%s', 必须为 0, 1 或 2。\n", program_name, optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
                print_help(program_name);
                return EXIT_SUCCESS;
            case 'u': uid_str = optarg; break;
            case 'g': gid_str = optarg; break;
            default:
                fprintf(stderr, "%s: 错误: 未知选项或缺少选项参数 '-%c'。\n", program_name, optopt);
                print_help(program_name);
                return EXIT_FAILURE;
        }
    }

    // 获取并设置 UID 和 GID
if (uid_str) {
    struct passwd *pwd = getpwnam(uid_str);
    if (pwd) {
        target_uid = pwd->pw_uid;
    } else {
        char *endptr;
        target_uid = strtol(uid_str, &endptr, 10);
        if (*endptr != '\0' || (target_uid == 0 && errno == EINVAL)) {
            fprintf(stderr, "%s: 错误: 无效的用户ID或用户名 '%s'\n", program_name, uid_str);
            return EXIT_FAILURE;
        }
    }
}

// 获取目标GID
if (gid_str) {
    struct group *grp = getgrnam(gid_str);
    if (grp) {
        target_gid = grp->gr_gid;
    } else {
        char *endptr;
        target_gid = strtol(gid_str, &endptr, 10);
        if (*endptr != '\0' || (target_gid == 0 && errno == EINVAL)) {
            fprintf(stderr, "%s: 错误: 无效的组ID或组名 '%s'\n", program_name, gid_str);
            return EXIT_FAILURE;
        }
    }
}

// 设置日志文件
log_file = fopen("run.log", "a");
if (!log_file) {
    perror("无法打开日志文件");
    return EXIT_FAILURE;
}

if (target_gid != -1) {
    if (setgid(target_gid) != 0) {
        fprintf(stderr, "%s: setgid(%u) 失败: %s\n", program_name, 
                (unsigned int)target_gid, strerror(errno));
        fclose(log_file);
        return EXIT_FAILURE;
    }
    log_message(2, "成功设置GID为: %u\n", (unsigned int)target_gid);
}

if (target_uid != -1) {
    if (setuid(target_uid) != 0) {
        fprintf(stderr, "%s: setuid(%u) 失败: %s\n", program_name, 
                (unsigned int)target_uid, strerror(errno));
        fclose(log_file);
        return EXIT_FAILURE;
    }
    log_message(2, "成功设置UID为: %u\n", (unsigned int)target_uid);
}

    time_t start_time = time(NULL);
    char time_str[100];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&start_time));
    log_message(1, "\n程序启动时间: %s\n", time_str);

    log_message(2, "调试信息: argc = %d, optind = %d\n", argc, optind);
    log_message(2, "调试信息: blacklist1_file = %s, blacklist2_file = %s, whitelist_file = %s, days = %d\n",
           blacklist1_file, blacklist2_file, whitelist_file, days);
    log_message(2, "调试信息: seconds = %d, debug_level = %d, target_uid_str = %s, target_gid_str = %s\n",
       seconds, debug_level, uid_str ? uid_str : "(未指定)", gid_str? gid_str : "(未指定)");

    if (!blacklist1_file || !whitelist_file) {
        log_message(1, "%s: 错误: 必须指定 -1 <blacklist1> 和 -w <whitelist> 文件路径。\n", program_name);
        print_help(program_name);
        fclose(log_file);
        return EXIT_FAILURE;
    }
    if (blacklist2_file && days <= 0) {
        log_message(1, "%s: 错误: 当指定 -2 <blacklist2> 时，必须同时指定 -D <days> 且天数必须为正整数。\n", program_name);
        print_help(program_name);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    do {
        time_t loop_start_time = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&loop_start_time));
        log_message(1, "\n循环开始时间: %s\n", time_str);

        char **blacklist1 = NULL, **blacklist2 = NULL, **whitelist = NULL;
        int bl1_count = read_file_to_array(blacklist1_file, &blacklist1);
        int bl2_count = blacklist2_file ? read_file_to_array(blacklist2_file, &blacklist2) : 0;
        int wl_count = read_file_to_array(whitelist_file, &whitelist);

        process_blacklist(blacklist1, bl1_count, whitelist, wl_count, 0, 0);
        if (blacklist2_file) {
            process_blacklist(blacklist2, bl2_count, whitelist, wl_count, 1, days);
        }

        free_array(blacklist1, bl1_count);
        free_array(blacklist2, bl2_count);
        free_array(whitelist, wl_count);

        time_t end_time = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&end_time));
        log_message(1, "%s 已删除文件数: %d\n", time_str, total_files_deleted);
        log_message(1, "%s 已删除目录数: %d\n", time_str, total_dirs_deleted);

        total_files_deleted = total_dirs_deleted = 0;

        if (seconds > 0) {
            log_message(1, "等待 %d 秒后进行下一次循环...\n", seconds);
            sleep(seconds);
        } else {
            log_message(1, "单次执行完成，程序退出。\n");
        }

    } while (seconds > 0);

    fclose(log_file);
    return EXIT_SUCCESS;
}