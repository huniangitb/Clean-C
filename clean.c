#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <regex.h>
#include <fnmatch.h>
#include <stdarg.h>

int debug_level = 1;
int total_files_deleted = 0;
int total_dirs_deleted = 0;
FILE *log_file;
char program_name[PATH_MAX];
uid_t target_uid = -1;
gid_t target_gid = -1;

#define MAX_LOG_SIZE (256 * 1024)

// 用于描述特殊规则的结构体，同时保存路径和转换后的正则表达式
typedef struct {
    char path[PATH_MAX];
    regex_t regex;
    int regex_valid;
} SpecialRule;

// 获取指定文件的大小，如果失败则返回 -1
long long get_file_size(const char *filename) {
    struct stat statbuf;
    return (stat(filename, &statbuf) == 0) ? statbuf.st_size : -1;
}

// 日志文件尺寸超过上限时进行滚动处理：重命名为 run.log.old 后重新创建 run.log
void rotate_log_file() {
    fclose(log_file);
    if (rename("run.log", "run.log.old") == 0) {
        if (debug_level >= 1)
            fprintf(stderr, "日志文件已重命名为 run.log.old\n");
    } else {
        fprintf(stderr, "日志文件重命名失败: %s\n", strerror(errno));
    }
    log_file = fopen("run.log", "a");
    if (!log_file) {
        perror("无法打开新的日志文件");
        exit(EXIT_FAILURE);
    }
    if (debug_level >= 1)
        fprintf(log_file, "已打开新的日志文件，程序继续运行。\n");
}

// 检查当前日志文件大小，如超过限额则执行日志滚动
void check_and_rotate_log() {
    if (get_file_size("run.log") > MAX_LOG_SIZE)
        rotate_log_file();
}

// 根据调试级别记录日志信息，支持 printf 格式化输出
void log_message(int level, const char *format, ...) {
    if (debug_level < level)
        return;
    check_and_rotate_log();
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fflush(log_file);
}

// 删除文件或目录项，并记录相关操作日志
static void delete_item(const char *path, int is_dir) {
    if (!path)
        return;
    int (*remove_func)(const char *) = is_dir ? rmdir : remove;
    if (remove_func(path) == 0) {
        if (is_dir) {
            total_dirs_deleted++;
            log_message(2, "已删除目录: %s\n", path);
        } else {
            total_files_deleted++;
            struct stat statbuf;
            int is_link = (lstat(path, &statbuf) == 0 && S_ISLNK(statbuf.st_mode));
            log_message(2, "已删除%s: %s\n", is_link ? "符号链接" : "文件", path);
        }
    } else {
        log_message(1, "删除%s失败: %s, 错误: %s\n", 
            is_dir ? "目录" : "文件/链接", path, strerror(errno));
    }
}

// 将通配符模式转换为正则表达式（支持 '*', '?', 等符号）
char *wildcard_to_regex(const char *wildcard) {
    if (!wildcard)
        return NULL;
    size_t len = strlen(wildcard);
    char *regex_str = malloc(len * 3 + 3); // 分配足够空间以容纳转换后的表达式
    if (!regex_str)
        return NULL;
    char *p = regex_str;
    *p++ = '^'; // 从头开始匹配
    for (size_t i = 0; i < len; i++) {
        switch (wildcard[i]) {
            case '*': *p++ = '.'; *p++ = '*'; break;
            case '?': *p++ = '.'; break;
            case '.': *p++ = '\\'; *p++ = '.'; break;
            case '[': *p++ = '['; break;
            case ']': *p++ = ']'; break;
            case '|': *p++ = '|'; break;
            case '{': *p++ = '('; break;
            case '}': *p++ = ')'; break;
            case ',': *p++ = '|'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            default: *p++ = wildcard[i];
        }
    }
    *p++ = '$'; // 结束匹配
    *p = '\0';
    return regex_str;
}

// 解析特殊规则字符串，提取路径和规则模式，转换为正则表达式
static int parse_special_rule(const char *rule_str, SpecialRule *rule) {
    if (!rule_str || !rule)
        return 0;
    memset(rule, 0, sizeof(SpecialRule));
    const char *p = rule_str;
    const char *path_end = rule_str;
    // 查找 '[' 作为规则模式的开始位置
    while (*p) {
        if (*p == '\\') {
            p++;
            if (*p) p++;
        } else if (*p == '[') {
            path_end = p;
            break;
        } else {
            p++;
        }
    }
    if (*p != '[')
        return 0;
    size_t path_len = path_end - rule_str;
    if (path_len >= PATH_MAX)
        return 0;
    strncpy(rule->path, rule_str, path_len);
    rule->path[path_len] = '\0';
    p++;  // 跳过 '['
    const char *regex_start = p;
    const char *regex_end = NULL;
    // 查找对应的 ']' 结束标记
    while (*p) {
        if (*p == '\\') { 
            p++; 
            if (*p) p++; 
        }
        else if (*p == ']') { 
            regex_end = p; 
            break; 
        }
        else { 
            p++; 
        }
    }
    if (!regex_end)
        return 0;
    size_t regex_len = regex_end - regex_start;
    char *regex_pattern = malloc(regex_len + 1);
    if (!regex_pattern) {
        log_message(1, "内存分配失败: regex_pattern\n");
        return 0;
    }
    strncpy(regex_pattern, regex_start, regex_len);
    regex_pattern[regex_len] = '\0';
    char *final_regex = wildcard_to_regex(regex_pattern);
    free(regex_pattern);
    if (!final_regex) {
        log_message(1, "通配符转换为正则表达式失败\n");
        return 0;
    }
    if (regcomp(&rule->regex, final_regex, REG_EXTENDED | REG_NOSUB) != 0) {
        log_message(1, "正则表达式编译失败: %s\n", final_regex);
        free(final_regex);
        return 0;
    }
    free(final_regex);
    rule->regex_valid = 1;
    return 1;
}

// 释放特殊规则中分配的正则表达式资源
void free_special_rule(SpecialRule *rule) {
    if (rule && rule->regex_valid) {
        regfree(&rule->regex);
        rule->regex_valid = 0;
    }
}

// 判断文件名是否满足正则表达式匹配
static int filename_matches_regex(const char *filename, regex_t *regex) {
    return (filename && regex && regexec(regex, filename, 0, NULL, 0) == 0);
}

/* 
   改进后的白名单判断函数：
   确保路径或者其子路径如果在白名单中，则受保护，不进行删除处理 
*/
int is_in_whitelist(const char *path, char **whitelist, int whitelist_count) {
    if (!path || !whitelist || whitelist_count <= 0)
        return 0;
    // 直接比较白名单中是否存在完整匹配
    for (int i = 0; i < whitelist_count; i++) {
        if (whitelist[i] && fnmatch(whitelist[i], path, FNM_PATHNAME) == 0)
            return 1;
    }
    // 检查当前路径是否为白名单指定目录的子目录
    size_t path_len = strlen(path);
    char path_with_slash[PATH_MAX];
    snprintf(path_with_slash, sizeof(path_with_slash), "%s/", path);
    for (int i = 0; i < whitelist_count; i++) {
        if (whitelist[i] && strncmp(whitelist[i], path_with_slash, strlen(path_with_slash)) == 0)
            return 1;
    }
    return 0;
}

/* 
   将配置文件内容按行读取，忽略空行和以 '#' 开头的注释行，
   每行代表一条规则并存入数组中 
*/
int read_file_to_array(const char *filename, char ***array) {
    if (!filename || !array)
        return 0;
    FILE *file = fopen(filename, "r");
    if (!file) {
        log_message(1, "无法打开文件: %s, 错误: %s\n", filename, strerror(errno));
        return 0;
    }
    char *line = NULL;
    size_t line_len = 0;
    ssize_t read;
    int count = 0, capacity = 10;
    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        log_message(1, "内存分配失败: temp_array\n");
        fclose(file);
        return 0;
    }
    while ((read = getline(&line, &line_len, file)) != -1) {
        if (read > 0 && line[read - 1] == '\n')
            line[read - 1] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                log_message(1, "内存重分配失败: new_array\n");
                for (int j = 0; j < count; j++)
                    free(temp_array[j]);
                free(temp_array); 
                free(line); 
                fclose(file);
                return 0;
            }
            temp_array = new_array;
        }
        temp_array[count] = strdup(line);
        if (!temp_array[count]) {
            log_message(1, "内存分配失败: temp_array[%d]\n", count);
            for (int j = 0; j < count; j++)
                free(temp_array[j]);
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
    log_message(2, "成功从 %s 中读取 %d 条规则\n", filename, count);
    return count;
}

// 检查某文件或目录自上次修改后的时间间隔是否超过指定天数
int is_expired(const char *path, int days) {
    if (!path || days < 0)
        return 0;
    struct stat statbuf;
    if (lstat(path, &statbuf) == 0) {
        time_t current_time = time(NULL);
        if (current_time == (time_t)-1) {
            log_message(1, "警告：获取当前时间失败\n");
            return 0;
        }
        double diff_time = difftime(current_time, statbuf.st_mtime);
        return (diff_time > (double)days * 24 * 3600);
    }
    return 0;
}

/* 
   函数声明：递归处理指定基路径下的所有条目，
   同时依据白名单、过期时间等规则进行删除操作 
*/
void process_recursive(const char *base_path, char **whitelist, int wl_count, int check_expiry, int days);

// 递归删除目录及其内容：适用于删除符合条件的目录或文件
void delete_directory_recursive(const char *path, char **whitelist, int wl_count,
    regex_t *regex, int check_expiry, int days, int skip_root) {
    if (!path)
        return;
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
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char full_path[PATH_MAX];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >= PATH_MAX) {
            log_message(1, "路径过长: %s/%s\n", path, entry->d_name);
            continue;
        }
        if (is_in_whitelist(full_path, whitelist, wl_count)) {
            log_message(2, "项目在白名单中，跳过: %s\n", full_path);
            continue;
        }
        struct stat statbuf;
        if (lstat(full_path, &statbuf) != 0) {
            log_message(1, "无法获取文件信息: %s, 错误: %s\n", full_path, strerror(errno));
            continue;
        }
        if (S_ISDIR(statbuf.st_mode)) {
            delete_directory_recursive(full_path, whitelist, wl_count, regex, check_expiry, days, 0);
        } else {
            const char *filename = strrchr(full_path, '/');
            filename = filename ? filename + 1 : full_path;
            if ((!regex || filename_matches_regex(filename, regex)) &&
                (!check_expiry || is_expired(full_path, days))) {
                delete_item(full_path, 0);
            }
        }
    }
    closedir(dir);
    if (!skip_root && !is_in_whitelist(path, whitelist, wl_count)) {
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
            if (is_empty)
                delete_item(path, 1);
        }
    }
}

// 根据黑名单规则（支持通配符与递归）处理目标文件和目录的删除
static void process_blacklist(char **blacklist, int count, char **whitelist, int wl_count,
    int check_expiry, int days) {
    if (!blacklist || count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        if (!blacklist[i])
            continue;
        char *target_path = strdup(blacklist[i]);
        if (!target_path) {
            log_message(1, "内存分配失败\n");
            continue;
        }
        // 去除路径末尾的斜杠
        size_t len = strlen(target_path);
        while (len > 0 && target_path[len-1] == '/') {
            target_path[len-1] = '\0';
            len--;
        }
        // 若目标路径本身在白名单中，则跳过
        if (is_in_whitelist(target_path, whitelist, wl_count)) {
            log_message(2, "跳过白名单项: %s\n", target_path);
            free(target_path);
            continue;
        }
        // 检查是否包含通配符
        if (strpbrk(target_path, "*?[") != NULL) {
            char base_path[PATH_MAX] = {0};
            char pattern_buffer[PATH_MAX] = {0};
            char *pattern = NULL;
            int is_recursive = 0;
            // 检查是否包含 "**" 用于递归匹配
            char *double_star = strstr(target_path, "**");
            if (double_star) {
                is_recursive = 1;
                size_t base_len = double_star - target_path;
                while (base_len > 0 && target_path[base_len-1] == '/')
                    base_len--;
                if (base_len == 0)
                    strcpy(base_path, ".");
                else {
                    strncpy(base_path, target_path, base_len);
                    base_path[base_len] = '\0';
                }
                char *after_stars = double_star + 2;
                while (*after_stars == '/')
                    after_stars++;
                if (*after_stars) {
                    strncpy(pattern_buffer, after_stars, sizeof(pattern_buffer)-1);
                    pattern = pattern_buffer;
                } else {
                    pattern = NULL;
                }
                // 遍历 base_path 下的所有目录及文件
                DIR *dir = opendir(base_path);
                if (dir) {
                    struct dirent *entry;
                    while ((entry = readdir(dir)) != NULL) {
                        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                            continue;
                        char full_path[PATH_MAX];
                        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
                        struct stat st;
                        if (lstat(full_path, &st) == 0) {
                            if (S_ISDIR(st.st_mode)) {
                                if (!is_in_whitelist(full_path, whitelist, wl_count)) {
                                    process_recursive(full_path, whitelist, wl_count, check_expiry, days);
                                    // 若未设置匹配模式或名称符合模式，则删除目录
                                    if (pattern == NULL || fnmatch(pattern, entry->d_name, FNM_PATHNAME) == 0)
                                        delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                                }
                            } else {
                                if (pattern == NULL || fnmatch(pattern, entry->d_name, FNM_PATHNAME) == 0) {
                                    if (!is_in_whitelist(full_path, whitelist, wl_count) &&
                                        (!check_expiry || is_expired(full_path, days)))
                                        delete_item(full_path, 0);
                                }
                            }
                        }
                    }
                    closedir(dir);
                }
            } else {
                // 处理不使用递归匹配的通配符模式
                char *last_slash = strrchr(target_path, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    pattern = last_slash + 1;
                    strncpy(base_path, target_path, sizeof(base_path)-1);
                } else {
                    strcpy(base_path, ".");
                    pattern = target_path;
                }
                DIR *dir = opendir(base_path);
                if (dir) {
                    struct dirent *entry;
                    while ((entry = readdir(dir)) != NULL) {
                        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                            continue;
                        char full_path[PATH_MAX];
                        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
                        if (fnmatch(pattern, entry->d_name, FNM_PATHNAME) == 0) {
                            if (!is_in_whitelist(full_path, whitelist, wl_count)) {
                                struct stat st;
                                if (lstat(full_path, &st) == 0) {
                                    if (S_ISDIR(st.st_mode))
                                        delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                                    else if (!check_expiry || is_expired(full_path, days))
                                        delete_item(full_path, 0);
                                }
                            }
                        }
                    }
                    closedir(dir);
                }
            }
        } else {
            // 处理不包含通配符的目标路径
            struct stat st;
            if (lstat(target_path, &st) == 0) {
                if (S_ISDIR(st.st_mode))
                    delete_directory_recursive(target_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                else if (!check_expiry || is_expired(target_path, days))
                    delete_item(target_path, 0);
            }
        }
        free(target_path);
    }
}

// 递归处理给定基目录下所有文件与目录，根据白名单和过期规则决定是否删除
void process_recursive(const char *base_path, char **whitelist, int wl_count, int check_expiry, int days) {
    DIR *dir = opendir(base_path);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
        if (!is_in_whitelist(full_path, whitelist, wl_count)) {
            struct stat st;
            if (lstat(full_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    process_recursive(full_path, whitelist, wl_count, check_expiry, days);
                    delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                } else if (!check_expiry || is_expired(full_path, days))
                    delete_item(full_path, 0);
            }
        }
    }
    closedir(dir);
}

// 释放保存规则的字符串数组
void free_array(char **array, int count) {
    if (!array)
        return;
    for (int i = 0; i < count; i++)
        free(array[i]);
    free(array);
}

// 打印程序使用说明和命令行选项
void print_help(const char *program_name) {
    printf("用法:\n");
    printf("  %s [选项]\n", program_name);
    printf("\n选项:\n");
    printf("  -1 <blacklist1>, --blacklist1=<blacklist1>  指定不检查过期时间的黑名单文件路径。\n");
    printf("  -2 <blacklist2>, --blacklist2=<blacklist2>  指定需要检查过期时间的黑名单文件路径。\n");
    printf("  -w <whitelist>, --whitelist=<whitelist>      指定白名单文件路径，每行一条规则。\n");
    printf("  -D <days>, --days=<days>                     设置文件过期天数（适用于 -2 选项，必须为非负整数）。\n");
    printf("  -s <seconds>, --seconds=<seconds>            设置循环执行的间隔时间（秒，0表示仅执行一次）。\n");
    printf("  -d <debug_level>, --debug=<debug_level>      设置调试级别 (0=无日志, 1=基础日志, 2=详细日志)。\n");
    printf("  -u <uid>, --uid=<uid>                        指定以特定用户ID或用户名运行。\n");
    printf("  -g <gid>, --gid=<gid>                        指定以特定组ID或组名运行。\n");
    printf("  -h, --help                                  显示帮助信息。\n");
    printf("\n注意:\n");
    printf("  - 黑名单和白名单文件中每行代表一条规则，支持注释（以 '#' 开头）以及空行。\n");
    printf("  - 黑名单规则支持通配符，并可用方括号指定模式，如: /tmp/cache/[*.tmp|*.log]\n");
    printf("  - 白名单规则应为完整路径，匹配该路径及其所有子目录/文件。\n");
    printf("\n示例:\n");
    printf("  %s -1 blacklist1.txt -w whitelist.txt -s 60 -d 1\n", program_name);
    printf("  %s -1 blacklist1.txt -2 blacklist2.txt -w whitelist.txt -D 30\n", program_name);
}

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

    int opt, seconds = 0, days = 0;
    char *uid_str = NULL, *gid_str = NULL, *blacklist1_file = NULL, *blacklist2_file = NULL, *whitelist_file = NULL;
    char time_str[100];

    while ((opt = getopt_long(argc, argv, "1:2:w:D:s:d:hu:g:", long_options, NULL)) != -1) {
        switch (opt) {
            case '1': 
                blacklist1_file = optarg; 
                break;
            case '2': 
                blacklist2_file = optarg; 
                break;
            case 'w': 
                whitelist_file = optarg; 
                break;
            case 'D':
                days = atoi(optarg);
                if (days < 0) {
                    fprintf(stderr, "%s: 错误: 无效的天数 '%s'\n", program_name, optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 's':
                seconds = atoi(optarg);
                if (seconds < 0) {
                    fprintf(stderr, "%s: 错误: 无效的时间间隔 '%s'\n", program_name, optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'd':
                debug_level = atoi(optarg);
                if (debug_level < 0 || debug_level > 2) {
                    fprintf(stderr, "%s: 错误: 无效的调试级别 '%s'\n", program_name, optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
                print_help(program_name);
                return EXIT_SUCCESS;
            case 'u':
                uid_str = optarg;
                break;
            case 'g':
                gid_str = optarg;
                break;
            default:
                fprintf(stderr, "%s: 错误: 未知选项 '-%c'\n", program_name, optopt);
                print_help(program_name);
                return EXIT_FAILURE;
        }
    }

    if (uid_str) {
        struct passwd *pwd = getpwnam(uid_str);
        if (pwd)
            target_uid = pwd->pw_uid;
        else {
            char *endptr;
            target_uid = strtol(uid_str, &endptr, 10);
            if (*endptr != '\0' || (target_uid == 0 && errno == EINVAL)) {
                fprintf(stderr, "%s: 错误: 无效的用户ID或用户名 '%s'\n", program_name, uid_str);
                return EXIT_FAILURE;
            }
        }
    }
    if (gid_str) {
        struct group *grp = getgrnam(gid_str);
        if (grp)
            target_gid = grp->gr_gid;
        else {
            char *endptr;
            target_gid = strtol(gid_str, &endptr, 10);
            if (*endptr != '\0' || (target_gid == 0 && errno == EINVAL)) {
                fprintf(stderr, "%s: 错误: 无效的组ID或组名 '%s'\n", program_name, gid_str);
                return EXIT_FAILURE;
            }
        }
    }

    log_file = fopen("run.log", "a");
    if (!log_file) {
        perror("无法打开日志文件");
        return EXIT_FAILURE;
    }

    if (target_gid != -1 && setgid(target_gid) != 0) {
        fprintf(stderr, "%s: 设置 GID (%u) 失败: %s\n", program_name,
                (unsigned int)target_gid, strerror(errno));
        fclose(log_file);
        return EXIT_FAILURE;
    }
    log_message(2, "已设置 GID: %u\n", (unsigned int)target_gid);

    if (target_uid != -1 && setuid(target_uid) != 0) {
        fprintf(stderr, "%s: 设置 UID (%u) 失败: %s\n", program_name,
                (unsigned int)target_uid, strerror(errno));
        fclose(log_file);
        return EXIT_FAILURE;
    }
    log_message(2, "已设置 UID: %u\n", (unsigned int)target_uid);

    time_t start_time = time(NULL);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&start_time));
    log_message(1, "\n程序启动时间: %s\n", time_str);

    log_message(2, "参数信息: blacklist1=%s, blacklist2=%s, whitelist=%s, days=%d, seconds=%d, debug=%d\n",
                blacklist1_file, blacklist2_file, whitelist_file, days, seconds, debug_level);

    if (!blacklist1_file || !whitelist_file) {
        log_message(1, "%s: 错误: 必须同时指定 -1 <blacklist1> 和 -w <whitelist> 文件路径。\n", program_name);
        print_help(program_name);
        fclose(log_file);
        return EXIT_FAILURE;
    }
    if (blacklist2_file && days <= 0) {
        log_message(1, "%s: 错误: 使用 -2 时必须同时设置 -D 且天数必须为正数。\n", program_name);
        print_help(program_name);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    do {
        time_t loop_start = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&loop_start));
        log_message(1, "\n【循环开始】时间: %s\n", time_str);

        char **blacklist1 = NULL, **blacklist2 = NULL, **whitelist = NULL;
        int bl1_count = read_file_to_array(blacklist1_file, &blacklist1);
        int bl2_count = blacklist2_file ? read_file_to_array(blacklist2_file, &blacklist2) : 0;
        int wl_count = read_file_to_array(whitelist_file, &whitelist);

        process_blacklist(blacklist1, bl1_count, whitelist, wl_count, 0, 0);
        if (blacklist2_file)
            process_blacklist(blacklist2, bl2_count, whitelist, wl_count, 1, days);

        free_array(blacklist1, bl1_count);
        free_array(blacklist2, bl2_count);
        free_array(whitelist, wl_count);

        time_t end_time = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&end_time));
        log_message(1, "%s 已删除文件数: %d\n", time_str, total_files_deleted);
        log_message(1, "%s 已删除目录数: %d\n", time_str, total_dirs_deleted);

        total_files_deleted = total_dirs_deleted = 0;
        if (seconds > 0) {
            log_message(1, "等待 %d 秒后继续下一次循环...\n", seconds);
            sleep(seconds);
        } else {
            log_message(1, "执行完成，程序退出。\n");
        }
    } while (seconds > 0);

    fclose(log_file);
    return EXIT_SUCCESS;
}
