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

int debug_level = 2; // 0: 关闭日志, 1: 一般信息, 2: 调试信息
int total_files_deleted = 0;
int total_dirs_deleted = 0;
FILE *log_file;

// 检查路径是否在白名单中
int is_in_whitelist(const char *path, char **whitelist, int whitelist_count) {
    for (int i = 0; i < whitelist_count; i++) {
        if (strchr(whitelist[i], '[') && strchr(whitelist[i], ']')) {
            char *pattern_start = strchr(whitelist[i], '[');
            size_t path_len = pattern_start - whitelist[i];
            if (path_len >= PATH_MAX) {
                continue;
            }
            char white_path[PATH_MAX];
            strncpy(white_path, whitelist[i], path_len);
            white_path[path_len] = '\0';

            char *pattern_end = strchr(pattern_start, ']');
            if (!pattern_end) {
                continue;
            }
            size_t pattern_len = pattern_end - pattern_start - 1;
            char white_patterns[pattern_len + 1];
            strncpy(white_patterns, pattern_start + 1, pattern_len);
            white_patterns[pattern_len] = '\0';

            if (fnmatch(white_path, path, FNM_PATHNAME) == 0) {
                const char *filename = strrchr(path, '/');
                if (filename) {
                    filename++;
                } else {
                    filename = path;
                }
                char *token = strtok(white_patterns, "|");
                while (token) {
                    if (fnmatch(token, filename, 0) == 0) {
                        return 1;
                    }
                    token = strtok(NULL, "|");
                }
            }
        } else {
            if (fnmatch(whitelist[i], path, FNM_PATHNAME) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

// 从文件读取列表到数组
int read_file_to_array(const char *filename, char ***array) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        if (debug_level >= 2) fprintf(log_file, "无法打开文件: %s, 错误: %s\n", filename, strerror(errno));
        return 0;
    }

    char line[4096];
    int count = 0;
    int capacity = 10;
    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        if (debug_level >= 2) fprintf(log_file, "内存分配失败\n");
        fclose(file);
        return 0;
    }

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                if (debug_level >= 2) fprintf(log_file, "内存重新分配失败\n");
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
            if (debug_level >= 2) fprintf(log_file, "字符串复制失败\n");
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

// 检查文件是否过期
int is_expired(const char *path, int days) {
    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
        time_t current_time = time(NULL);
        double diff_time = difftime(current_time, statbuf.st_mtime);
        return (diff_time > days * 24 * 3600);
    }
    return 0;
}

// 递归删除目录及其内容
void delete_directory_recursive(const char *path, char **whitelist, int whitelist_count, const char **patterns, int pattern_count) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (debug_level >= 2) fprintf(log_file, "无法打开目录: %s, 错误: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (!is_in_whitelist(full_path, whitelist, whitelist_count)) {
            struct stat statbuf;
            if (lstat(full_path, &statbuf) != 0) {
                if (debug_level >= 2) fprintf(log_file, "无法获取文件信息: %s, 错误: %s\n", full_path, strerror(errno));
                continue;
            }

            if (S_ISDIR(statbuf.st_mode)) {
                delete_directory_recursive(full_path, whitelist, whitelist_count, patterns, pattern_count);
                if (rmdir(full_path) == 0) {
                    total_dirs_deleted++;
                    if (debug_level >= 2) fprintf(log_file, "删除目录: %s\n", full_path);
                } else {
                    if (debug_level >= 2) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", full_path, strerror(errno));
                }
            } else if (S_ISREG(statbuf.st_mode) || S_ISLNK(statbuf.st_mode)) {
                int match = 0;
                if (patterns && pattern_count > 0) {
                    for (int i = 0; i < pattern_count; i++) {
                        if (fnmatch(patterns[i], entry->d_name, 0) == 0) {
                            match = 1;
                            break;
                        }
                    }
                } else {
                    match = 1;
                }

                if (match) {
                    if (remove(full_path) == 0) {
                        total_files_deleted++;
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (debug_level >= 2) fprintf(log_file, "删除符号链接: %s\n", full_path);
                        } else {
                            if (debug_level >= 2) fprintf(log_file, "删除文件: %s\n", full_path);
                        }
                    } else {
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (debug_level >= 2) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", full_path, strerror(errno));
                        } else {
                            if (debug_level >= 2) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", full_path, strerror(errno));
                        }
                    }
                }
            }
        }
    }

    closedir(dir);
}

// 处理黑名单1
void process_blacklist1(char **blacklist, int blacklist_count, char **whitelist, int whitelist_count) {
    for (int i = 0; i < blacklist_count; i++) {
        if (strchr(blacklist[i], '[') && strchr(blacklist[i], ']')) {
            char *pattern_start = strchr(blacklist[i], '[');
            size_t path_len = pattern_start - blacklist[i];
            if (path_len >= PATH_MAX) {
                if (debug_level >= 2) fprintf(log_file, "路径过长: %s\n", blacklist[i]);
                continue;
            }
            char path[PATH_MAX];
            strncpy(path, blacklist[i], path_len);
            path[path_len] = '\0';

            char *pattern_end = strchr(pattern_start, ']');
            if (!pattern_end) {
                if (debug_level >= 2) fprintf(log_file, "无效的特殊规则（缺少 ']'): %s\n", blacklist[i]);
                continue;
            }
            size_t pattern_len = pattern_end - pattern_start - 1;
            char patterns[pattern_len + 1];
            strncpy(patterns, pattern_start + 1, pattern_len);
            patterns[pattern_len] = '\0';

            int pattern_capacity = 10;
            int pattern_count = 0;
            const char **pattern_array = malloc(sizeof(char*) * pattern_capacity);
            if (!pattern_array) {
                if (debug_level >= 2) fprintf(log_file, "内存分配失败\n");
                continue;
            }

            char *token = strtok(patterns, "|");
            while (token) {
                if (pattern_count >= pattern_capacity) {
                    pattern_capacity *= 2;
                    pattern_array = realloc(pattern_array, sizeof(char*) * pattern_capacity);
                    if (!pattern_array) {
                        if (debug_level >= 2) fprintf(log_file, "内存重新分配失败\n");
                        break;
                    }
                }
                pattern_array[pattern_count++] = token;
                token = strtok(NULL, "|");
            }

            if (pattern_count > 0) {
                delete_directory_recursive(path, whitelist, whitelist_count, pattern_array, pattern_count);
                free(pattern_array);
            }
        } else {
            if (!is_in_whitelist(blacklist[i], whitelist, whitelist_count)) {
                struct stat statbuf;
                if (lstat(blacklist[i], &statbuf) != 0) {
                    if (debug_level >= 2) fprintf(log_file, "路径不存在，跳过: %s\n", blacklist[i]);
                    continue;
                }

                if (S_ISDIR(statbuf.st_mode)) {
                    delete_directory_recursive(blacklist[i], whitelist, whitelist_count, NULL, 0);
                    if (rmdir(blacklist[i]) == 0) {
                        total_dirs_deleted++;
                        if (debug_level >= 2) fprintf(log_file, "删除目录: %s\n", blacklist[i]);
                    } else {
                        if (debug_level >= 2) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                    }
                } else {
                    if (remove(blacklist[i]) == 0) {
                        total_files_deleted++;
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (debug_level >= 2) fprintf(log_file, "删除符号链接: %s\n", blacklist[i]);
                        } else {
                            if (debug_level >= 2) fprintf(log_file, "删除文件: %s\n", blacklist[i]);
                        }
                    } else {
                        if (S_ISLNK(statbuf.st_mode)) {
                            if (debug_level >= 2) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                        } else {
                            if (debug_level >= 2) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
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
        if (strchr(blacklist[i], '[') && strchr(blacklist[i], ']')) {
            char *pattern_start = strchr(blacklist[i], '[');
            size_t path_len = pattern_start - blacklist[i];
            if (path_len >= PATH_MAX) {
                if (debug_level >= 2) fprintf(log_file, "路径过长: %s\n", blacklist[i]);
                continue;
            }
            char path[PATH_MAX];
            strncpy(path, blacklist[i], path_len);
            path[path_len] = '\0';

            char *pattern_end = strchr(pattern_start, ']');
            if (!pattern_end) {
                if (debug_level >= 2) fprintf(log_file, "无效的特殊规则（缺少 ']'): %s\n", blacklist[i]);
                continue;
            }
            size_t pattern_len = pattern_end - pattern_start - 1;
            char patterns[pattern_len + 1];
            strncpy(patterns, pattern_start + 1, pattern_len);
            patterns[pattern_len] = '\0';

            int pattern_capacity = 10;
            int pattern_count = 0;
            const char **pattern_array = malloc(sizeof(char*) * pattern_capacity);
            if (!pattern_array) {
                if (debug_level >= 2) fprintf(log_file, "内存分配失败\n");
                continue;
            }

            char *token = strtok(patterns, "|");
            while (token) {
                if (pattern_count >= pattern_capacity) {
                    pattern_capacity *= 2;
                    pattern_array = realloc(pattern_array, sizeof(char*) * pattern_capacity);
                    if (!pattern_array) {
                        if (debug_level >= 2) fprintf(log_file, "内存重新分配失败\n");
                        break;
                    }
                }
                pattern_array[pattern_count++] = token;
                token = strtok(NULL, "|");
            }

            if (pattern_count > 0) {
                delete_directory_recursive(path, whitelist, whitelist_count, pattern_array, pattern_count);
                free(pattern_array);
            }
        } else {
            if (!is_in_whitelist(blacklist[i], whitelist, whitelist_count)) {
                struct stat statbuf;
                if (lstat(blacklist[i], &statbuf) != 0) {
                    if (debug_level >= 2) fprintf(log_file, "路径不存在，跳过: %s\n", blacklist[i]);
                    continue;
                }

                if (S_ISDIR(statbuf.st_mode)) {
                    delete_directory_recursive(blacklist[i], whitelist, whitelist_count, NULL, 0);
                    if (rmdir(blacklist[i]) == 0) {
                        total_dirs_deleted++;
                        if (debug_level >= 2) fprintf(log_file, "删除目录: %s\n", blacklist[i]);
                    } else {
                        if (debug_level >= 2) fprintf(log_file, "删除目录失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                    }
                } else {
                    if (is_expired(blacklist[i], days)) {
                        if (remove(blacklist[i]) == 0) {
                            total_files_deleted++;
                            if (S_ISLNK(statbuf.st_mode)) {
                                if (debug_level >= 2) fprintf(log_file, "删除符号链接: %s\n", blacklist[i]);
                            } else {
                                if (debug_level >= 2) fprintf(log_file, "删除文件: %s\n", blacklist[i]);
                            }
                        } else {
                            if (S_ISLNK(statbuf.st_mode)) {
                                if (debug_level >= 2) fprintf(log_file, "删除符号链接失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                            } else {
                                if (debug_level >= 2) fprintf(log_file, "删除文件失败: %s, 错误: %s\n", blacklist[i], strerror(errno));
                            }
                        }
                    }
                }
            }
        }
    }
}

// 释放数组内存
void free_array(char **array, int count) {
    for (int i = 0; i < count; i++) {
        free(array[i]);
    }
    free(array);
}

int main(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"seconds", required_argument, 0, 's'},
        {"debug", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int seconds = 0;

    while ((opt = getopt_long(argc, argv, "s:d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                seconds = atoi(optarg);
                break;
            case 'd':
                if (strcmp(optarg, "0") == 0) {
                    debug_level = 0;
                } else if (strcmp(optarg, "1") == 0) {
                    debug_level = 1;
                } else if (strcmp(optarg, "2") == 0) {
                    debug_level = 2;
                } else {
                    fprintf(stderr, "未知的debug参数: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
                printf("用法: %s <blacklist1_file> <blacklist2_file> <whitelist_file> <过期天数> [选项]\n", argv[0]);
                printf("选项:\n");
                printf("  -s, --seconds <秒数>    设置重复执行间隔\n");
                printf("  -d, --debug <0|1|2>         设置日志级别，默认为1\n");
                printf("  -h, --help                 帮助页面\n");
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "用法: %s <blacklist1_file> <blacklist2_file> <whitelist_file> <days> [选项]\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (argc - optind < 4) {
        fprintf(stderr, "参数不足，正确使用方法: %s <blacklist1_file> <blacklist2_file> <whitelist_file> <days> [选项]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *blacklist1_file = argv[optind];
    char *blacklist2_file = argv[optind + 1];
    char *whitelist_file = argv[optind + 2];
    int days = atoi(argv[optind + 3]);

    do {
        log_file = fopen("log.txt", "a");
        if (!log_file) {
            perror("无法打开或创建日志文件");
            return EXIT_FAILURE;
        }

        time_t start_time = time(NULL);
        char time_str[100];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&start_time));
        if (debug_level >= 1) fprintf(log_file, "程序启动时间: %s\n", time_str);

        char **blacklist1 = NULL, **blacklist2 = NULL, **whitelist = NULL;
        int blacklist1_count = read_file_to_array(blacklist1_file, &blacklist1);
        int blacklist2_count = read_file_to_array(blacklist2_file, &blacklist2);
        int whitelist_count = read_file_to_array(whitelist_file, &whitelist);

        process_blacklist1(blacklist1, blacklist1_count, whitelist, whitelist_count);
        process_blacklist2(blacklist2, blacklist2_count, whitelist, whitelist_count, days);

        free_array(blacklist1, blacklist1_count);
        free_array(blacklist2, blacklist2_count);
        free_array(whitelist, whitelist_count);

        time_t end_time = time(NULL);
        char end_time_str[100];
        strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S", localtime(&end_time));
        if (debug_level >= 1) fprintf(log_file, "程序结束时间: %s\n", end_time_str);
        if (debug_level >= 1) fprintf(log_file, "已删除文件数: %d\n", total_files_deleted);
        if (debug_level >= 1) fprintf(log_file, "已删除目录数: %d\n", total_dirs_deleted);
        if (debug_level >= 1) fprintf(log_file, "================\n");
        total_files_deleted = 0;
        total_dirs_deleted = 0;

        fclose(log_file);

        if (seconds > 0) {
            sleep(seconds);
        }
    } while (seconds > 0);

    return EXIT_SUCCESS;
}
