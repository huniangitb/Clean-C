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

// 全局变量
int logging_enabled = 1;
int total_files_deleted = 0;
int total_dirs_deleted = 0;
FILE *log_file;

// 判断路径是否在白名单中
int is_in_whitelist(const char *path, char **whitelist, int whitelist_count) {
    for (int i = 0; i < whitelist_count; i++) {
        size_t wl_len = strlen(whitelist[i]);
        if (strcmp(path, whitelist[i]) == 0 ||
            (strncmp(path, whitelist[i], wl_len) == 0 && path[wl_len] == '/')) {
            return 1; // 在白名单中
        }
    }
    return 0; // 不在白名单中
}

// 读取文件内容到动态数组，并忽略以 '#' 开头的行
int read_file_to_array(const char *filename, char ***array) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        if (logging_enabled) fprintf(log_file, "无法打开文件: %s, 错误: %s\n", filename, strerror(errno));
        return 0;
    }

    char line[4096];
    int count = 0;
    int capacity = 10;
    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        if (logging_enabled) fprintf(log_file, "内存分配失败\n");
        fclose(file);
        return 0;
    }

    while (fgets(line, sizeof(line), file)) {
        // 忽略以 '#' 开头的行（注释）和空行
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        line[strcspn(line, "\n")] = '\0'; // 去掉换行符
        if (strlen(line) == 0) {
            continue; // 忽略空行
        }

        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                if (logging_enabled) fprintf(log_file, "内存重新分配失败\n");
                for (int j = 0; j < count; j++) {
                    free(temp_array[j]);
                }
                free(temp_array);
                fclose(file);
                return 0;
            }
            temp_array = new_array;
        }
        temp_array[count] = strdup(line);
        if (!temp_array[count]) {
            if (logging_enabled) fprintf(log_file, "字符串复制失败\n");
            for (int j = 0; j < count; j++) {
                free(temp_array[j]);
            }
            free(temp_array);
            fclose(file);
            return 0;
        }
        count++;
    }

    fclose(file);
    *array = temp_array;
    return count;
}

// 判断文件是否过期
int is_expired(const char *path, int days) {
    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
        time_t current_time = time(NULL);
        double diff_time = difftime(current_time, statbuf.st_mtime);
        return (diff_time > days * 24 * 3600); // 超过指定天数
    }
    // 如果无法获取文件信息，假设不过期
    return 0;
}

// 递归删除目录下的所有文件和子目录
void delete_directory_recursive(const char *path, char **whitelist, int whitelist_count, const char **patterns, int pattern_count) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (logging_enabled) fprintf(log_file, "无法打开目录: %s, 错误: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // 跳过当前目录和上级目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >= sizeof(full_path)) {
            if (logging_enabled) fprintf(log_file, "路径过长，无法处理: %s/%s\n", path, entry->d_name);
            continue;
        }

        // 判断是否匹配模式（如果有模式）
        int matches_pattern = 1;
        if (patterns && pattern_count > 0) {
            matches_pattern = 0;
            for (int i = 0; i < pattern_count; i++) {
                if (fnmatch(patterns[i], entry->d_name, 0) == 0) {
                    matches_pattern = 1;
                    break;
                }
            }
        }

        if (!is_in_whitelist(full_path, whitelist, whitelist_count) && matches_pattern) {
            struct stat statbuf;
            if (lstat(full_path, &statbuf) != 0) {
                if (logging_enabled) fprintf(log_file, "无法获取文件信息: %s, 错误: %s\n", full_path, strerror(errno));
                continue;
            }

            if (S_ISDIR(statbuf.st_mode)) {
                // 递归删除子目录
                delete_directory_recursive(full_path, whitelist, whitelist_count, patterns, pattern_count);
                if (rmdir(full_path) == 0) {
                    total_dirs_deleted++;
                    if (logging_enabled) fprintf(log_file, "删除目录: %s\n", full_path);
                } else {
                    if (logging_enabled) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", full_path, strerror(errno));
                }
            } else if (S_ISREG(statbuf.st_mode) || S_ISLNK(statbuf.st_mode)) {
                // 删除文件或符号链接
                if (remove(full_path) == 0) {
                    total_files_deleted++;
                    if (S_ISLNK(statbuf.st_mode)) {
                        if (logging_enabled) fprintf(log_file, "删除符号链接: %s\n", full_path);
                    } else {
                        if (logging_enabled) fprintf(log_file, "删除文件: %s\n", full_path);
                    }
                } else {
                    if (S_ISLNK(statbuf.st_mode)) {
                        if (logging_enabled) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", full_path, strerror(errno));
                    } else {
                        if (logging_enabled) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", full_path, strerror(errno));
                    }
                }
            } else {
                // 处理其他文件类型（FIFO, 套接字等）根据需求
                if (remove(full_path) == 0) {
                    total_files_deleted++;
                    if (logging_enabled) fprintf(log_file, "删除特殊文件: %s\n", full_path);
                } else {
                    if (logging_enabled) fprintf(log_file, "删除特殊文件失败: %s, 错误: %s\n", full_path, strerror(errno));
                }
            }
        }
    }

    closedir(dir);
}

// 处理黑名单1
void process_blacklist1(char **blacklist, int blacklist_count, char **whitelist, int whitelist_count) {
    for (int i = 0; i < blacklist_count; i++) {
        size_t len = strlen(blacklist[i]);
        if (len == 0) continue;

        // 检查最后一个字符是否为 '*'
        if (blacklist[i][len - 1] == '*') {
            // 去除 '*' 字符，保留路径
            if (len < 2 || blacklist[i][len - 2] != '/') {
                if (logging_enabled) fprintf(log_file, "无效的黑名单模式: %s\n", blacklist[i]);
                continue;
            }

            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%.*s", (int)(len - 1), blacklist[i]) >= sizeof(path)) {
                if (logging_enabled) fprintf(log_file, "路径过长，无法处理: %s\n", blacklist[i]);
                continue;
            }

            // 判断路径是否存在
            struct stat statbuf;
            if (stat(path, &statbuf) != 0) {
                if (logging_enabled) fprintf(log_file, "路径不存在，跳过: %s\n", path);
                continue;
            }

            delete_directory_recursive(path, whitelist, whitelist_count, NULL, 0);
            if (rmdir(path) == 0) {
                total_dirs_deleted++;
                if (logging_enabled) fprintf(log_file, "删除目录: %s\n", path);
            } else {
                if (logging_enabled) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", path, strerror(errno));
            }
        } else {
            // 删除单个文件或目录
            if (!is_in_whitelist(blacklist[i], whitelist, whitelist_count)) {
                struct stat statbuf;
                if (lstat(blacklist[i], &statbuf) != 0) {
                    if (logging_enabled) fprintf(log_file, "路径不存在，跳过: %s\n", blacklist[i]);
                    continue;
                }

                if (S_ISDIR(statbuf.st_mode)) {
                    delete_directory_recursive(blacklist[i], whitelist, whitelist_count, NULL, 0);
                    if (rmdir(blacklist[i]) == 0) {
                        total_dirs_deleted++;
                        if (logging_enabled) fprintf(log_file, "删除目录: %s\n", blacklist[i]);
                    } else {
                        if (logging_enabled) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                    }
                } else {
                    if (remove(blacklist[i]) == 0) {
                        total_files_deleted++;
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (logging_enabled) fprintf(log_file, "删除符号链接: %s\n", blacklist[i]);
                        } else {
                            if (logging_enabled) fprintf(log_file, "删除文件: %s\n", blacklist[i]);
                        }
                    } else {
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (logging_enabled) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                        } else {
                            if (logging_enabled) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                        }
                    }
                }
            }
        }
    }
}

// 处理黑名单2
void process_blacklist2(char **blacklist, int blacklist_count, char **whitelist, int whitelist_count, int days) {
    for (int i = 0; i < blacklist_count; i++) {
        size_t len = strlen(blacklist[i]);
        if (len == 0) continue;

        // 检查最后一个字符是否为 '*'
        if (blacklist[i][len - 1] == '*') {
            // 去除 '*' 字符，保留路径
            if (len < 2 || blacklist[i][len - 2] != '/') {
                if (logging_enabled) fprintf(log_file, "无效的黑名单模式: %s\n", blacklist[i]);
                continue;
            }

            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%.*s", (int)(len - 1), blacklist[i]) >= sizeof(path)) {
                if (logging_enabled) fprintf(log_file, "路径过长，无法处理: %s\n", blacklist[i]);
                continue;
            }

            // 判断路径是否存在
            struct stat statbuf;
            if (stat(path, &statbuf) != 0) {
                if (logging_enabled) fprintf(log_file, "路径不存在，跳过: %s\n", path);
                continue;
            }

            DIR *dir = opendir(path);
            if (!dir) {
                if (logging_enabled) fprintf(log_file, "无法打开目录: %s, 错误: %s\n", path, strerror(errno));
                continue;
            }

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                // 跳过当前目录和上级目录
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                char full_path[PATH_MAX];
                if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >= sizeof(full_path)) {
                    if (logging_enabled) fprintf(log_file, "路径过长，无法处理: %s/%s\n", path, entry->d_name);
                    continue;
                }

                if (is_expired(full_path, days) && !is_in_whitelist(full_path, whitelist, whitelist_count)) {
                    struct stat statbuf_entry;
                    if (lstat(full_path, &statbuf_entry) != 0) {
                        if (logging_enabled) fprintf(log_file, "无法获取文件信息: %s, 错误: %s\n", full_path, strerror(errno));
                        continue;
                    }

                    if (S_ISDIR(statbuf_entry.st_mode)) {
                        delete_directory_recursive(full_path, whitelist, whitelist_count, NULL, 0);
                        if (rmdir(full_path) == 0) {
                            total_dirs_deleted++;
                            if (logging_enabled) fprintf(log_file, "删除目录: %s\n", full_path);
                        } else {
                            if (logging_enabled) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", full_path, strerror(errno));
                        }
                    } else {
                        if (remove(full_path) == 0) {
                            total_files_deleted++;
                            if (S_ISLNK(statbuf_entry.st_mode)) {
                                if (logging_enabled) fprintf(log_file, "删除符号链接: %s\n", full_path);
                            } else {
                                if (logging_enabled) fprintf(log_file, "删除文件: %s\n", full_path);
                            }
                        } else {
                            if (S_ISLNK(statbuf_entry.st_mode)) {
                                if (logging_enabled) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", full_path, strerror(errno));
                            } else {
                                if (logging_enabled) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", full_path, strerror(errno));
                            }
                        }
                    }
                }
            }

            closedir(dir);
        } else {
            if (is_expired(blacklist[i], days) && !is_in_whitelist(blacklist[i], whitelist, whitelist_count)) {
                struct stat statbuf;
                if (lstat(blacklist[i], &statbuf) != 0) {
                    if (logging_enabled) fprintf(log_file, "路径不存在，跳过: %s\n", blacklist[i]);
                    continue;
                }

                if (S_ISDIR(statbuf.st_mode)) {
                    delete_directory_recursive(blacklist[i], whitelist, whitelist_count, NULL, 0);
                    if (rmdir(blacklist[i]) == 0) {
                        total_dirs_deleted++;
                        if (logging_enabled) fprintf(log_file, "删除目录: %s\n", blacklist[i]);
                    } else {
                        if (logging_enabled) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                    }
                } else {
                    if (remove(blacklist[i]) == 0) {
                        total_files_deleted++;
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (logging_enabled) fprintf(log_file, "删除符号链接: %s\n", blacklist[i]);
                        } else {
                            if (logging_enabled) fprintf(log_file, "删除文件: %s\n", blacklist[i]);
                        }
                    } else {
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (logging_enabled) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                        } else {
                            if (logging_enabled) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                        }
                    }
                }
            }
        }
    }
}

// 用于处理特定规则的函数
void process_special_rules(char **blacklist, int blacklist_count, char **whitelist, int whitelist_count) {
    for (int i = 0; i < blacklist_count; i++) {
        char *pattern_list = strstr(blacklist[i], "[");
        if (pattern_list) {
            // 确保 '[' 前有至少一个字符
            if (pattern_list == blacklist[i]) {
                if (logging_enabled) fprintf(log_file, "无效的特殊规则: %s\n", blacklist[i]);
                continue;
            }

            // 提取路径
            size_t path_len = pattern_list - blacklist[i] - 1; // 假设模式前有 '/'
            if (path_len <= 0 || path_len >= PATH_MAX) {
                if (logging_enabled) fprintf(log_file, "路径过长或无效: %s\n", blacklist[i]);
                continue;
            }

            char path[PATH_MAX];
            memcpy(path, blacklist[i], path_len);
            path[path_len] = '\0';

            // 提取模式
            char *patterns = pattern_list + 1;
            char *end_bracket = strstr(patterns, "]");
            if (!end_bracket) {
                if (logging_enabled) fprintf(log_file, "无效的特殊规则（缺少 ']'): %s\n", blacklist[i]);
                continue;
            }
            *end_bracket = '\0'; // 截断字符串

            // 分割模式，假设使用 '|' 作为分隔符
            int pattern_capacity = 10;
            int pattern_count = 0;
            const char **pattern_array = malloc(sizeof(char*) * pattern_capacity);
            if (!pattern_array) {
                if (logging_enabled) fprintf(log_file, "内存分配失败\n");
                continue;
            }

            char *token = strtok(patterns, "|");
            while (token) {
                if (pattern_count >= pattern_capacity) {
                    pattern_capacity *= 2;
                    const char **new_patterns = realloc(pattern_array, sizeof(char*) * pattern_capacity);
                    if (!new_patterns) {
                        if (logging_enabled) fprintf(log_file, "内存重新分配失败\n");
                        break;
                    }
                    pattern_array = new_patterns;
                }
                pattern_array[pattern_count++] = token;
                token = strtok(NULL, "|");
            }

            if (pattern_count > 0) {
                // 判断路径是否存在
                struct stat statbuf;
                if (stat(path, &statbuf) != 0) {
                    if (logging_enabled) fprintf(log_file, "路径不存在，跳过: %s\n", path);
                    free(pattern_array);
                    continue;
                }

                delete_directory_recursive(path, whitelist, whitelist_count, pattern_array, pattern_count);
            }

            free(pattern_array);
        } else {
            // 如果没有特殊规则，直接删除路径
            // 判断路径是否存在
            struct stat statbuf;
            if (stat(blacklist[i], &statbuf) != 0) {
                if (logging_enabled) fprintf(log_file, "路径不存在，跳过: %s\n", blacklist[i]);
                continue;
            }

            delete_directory_recursive(blacklist[i], whitelist, whitelist_count, NULL, 0);
            if (S_ISDIR(statbuf.st_mode)) {
                if (rmdir(blacklist[i]) == 0) {
                    total_dirs_deleted++;
                    if (logging_enabled) fprintf(log_file, "删除目录: %s\n", blacklist[i]);
                } else {
                    if (logging_enabled) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                }
            } else {
                if (remove(blacklist[i]) == 0) {
                    total_files_deleted++;
                    if (S_ISLNK(statbuf.st_mode)) {
                        if (logging_enabled) fprintf(log_file, "删除符号链接: %s\n", blacklist[i]);
                    } else {
                        if (logging_enabled) fprintf(log_file, "删除文件: %s\n", blacklist[i]);
                    }
                } else {
                    if (S_ISLNK(statbuf.st_mode)) {
                        if (logging_enabled) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                    } else {
                        if (logging_enabled) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                    }
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    // 定义长选项
    static struct option long_options[] = {
        {"seconds", required_argument, 0, 's'},
        {"debug", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int seconds = 0; // 默认不循环
    logging_enabled = 1; // 默认启用日志

    // 解析选项
    while ((opt = getopt_long(argc, argv, "s:d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                seconds = atoi(optarg);
                if (seconds < 0) {
                    fprintf(stderr, "秒数参数无效: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'd':
                if (strcmp(optarg, "off") == 0) {
                    logging_enabled = 0;
                } else if (strcmp(optarg, "on") != 0) {
                    fprintf(stderr, "未知的debug参数: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
                printf("用法: %s <blacklist1_file> <blacklist2_file> <whitelist_file> <过期天数> [选项]\n", argv[0]);
                printf("选项:\n");
                printf("  -s, --seconds <秒数>    设置重复执行间隔\n");
                printf("  -d, --debug <on|off>       启用debug模式,默认启用\n");
                printf("  -h, --help                 帮助页面\n");
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "用法: %s <blacklist1_file> <blacklist2_file> <whitelist_file> <days> [options]\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    // 余下的必需参数检查
    if (argc - optind < 4) {
        fprintf(stderr, "参数不足，正确使用方法: %s <blacklist1_file> <blacklist2_file> <whitelist_file> <days> [options]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *blacklist1_file = argv[optind];
    char *blacklist2_file = argv[optind + 1];
    char *whitelist_file = argv[optind + 2];
    int days = atoi(argv[optind + 3]);
    if (days < 0) {
        fprintf(stderr, "天数参数无效: %s\n", argv[optind + 3]);
        return EXIT_FAILURE;
    }

    // 进入循环执行
    do {
        // 打开日志文件（追加模式），若不存在则创建
        log_file = fopen("log.txt", "a");
        if (!log_file) {
            perror("无法打开或创建日志文件");
            return EXIT_FAILURE;
        }

        // 记录程序启动
        time_t start_time = time(NULL);
        char time_str[100];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&start_time));
        if (logging_enabled) fprintf(log_file, "程序启动时间: %s\n", time_str);

        // 读取黑名单和白名单
        char **blacklist1 = NULL, **blacklist2 = NULL, **whitelist = NULL;
        int blacklist1_count = read_file_to_array(blacklist1_file, &blacklist1);
        int blacklist2_count = read_file_to_array(blacklist2_file, &blacklist2);
        int whitelist_count = read_file_to_array(whitelist_file, &whitelist);

        // 处理黑名单1和黑名单2
        process_blacklist1(blacklist1, blacklist1_count, whitelist, whitelist_count);
        process_blacklist2(blacklist2, blacklist2_count, whitelist, whitelist_count, days);

        // 处理特殊规则
        process_special_rules(blacklist1, blacklist1_count, whitelist, whitelist_count);

        // 输出删除统计信息
        if (logging_enabled || total_files_deleted > 0 || total_dirs_deleted > 0) {
            // 获取当前时间
            time_t now = time(NULL);
            char time_str_end[100];
            strftime(time_str_end, sizeof(time_str_end), "%Y-%m-%d %H:%M:%S", localtime(&now));

            // 输出当前时间信息
            fprintf(log_file, "当前时间: %s\n", time_str_end);

            fprintf(log_file, "已删除文件数: %d\n", total_files_deleted);
            fprintf(log_file, "已删除目录数: %d\n", total_dirs_deleted);
            fprintf(log_file, "================\n");
            total_files_deleted = 0;
            total_dirs_deleted = 0;
        }

        // 记录程序结束时间
        time_t end_time = time(NULL);
        char end_time_str[100];
        strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S", localtime(&end_time));
        if (logging_enabled) fprintf(log_file, "程序结束时间: %s\n", end_time_str);
        if (logging_enabled) fprintf(log_file, "================\n");

        // 释放分配的内存
        for (int i = 0; i < blacklist1_count; i++) {
            free(blacklist1[i]);
        }
        for (int i = 0; i < blacklist2_count; i++) {
            free(blacklist2[i]);
        }
        for (int i = 0; i < whitelist_count; i++) {
            free(whitelist[i]);
        }
        free(blacklist1);
        free(blacklist2);
        free(whitelist);

        // 关闭日志文件
        fclose(log_file);

        // 如果需要循环执行，等待指定的秒数
        if (seconds > 0) {
            sleep(seconds);
        }
    } while (seconds > 0);

    return EXIT_SUCCESS;
}
