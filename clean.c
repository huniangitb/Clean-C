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
#include <regex.h>
#include <fnmatch.h>
#include <stdarg.h>

// --- 全局变量 ---
int debug_level = 1;
int total_files_deleted = 0;
int total_dirs_deleted = 0;
long long total_bytes_deleted_this_cycle = 0;
FILE *log_file = NULL;
char program_name[PATH_MAX];

#define MAX_LOG_SIZE (256 * 1024) // 256 KB

// --- 新增：应用清理功能相关 ---
#define APP_CLEAN_PREFIX "/data/user/0/"
#define HASH_TABLE_SIZE 257 // 使用一个素数作为哈希表大小，以获得更好的分布
FILE *app_log_file = NULL; // 应用清理日志文件句柄

// --- 新增：哈希表节点结构 (用于存储每个应用的清理统计) ---
typedef struct AppCleanStatNode {
    char *packageName;
    long long totalBytes;
    struct AppCleanStatNode *next; // 用于处理哈希冲突的链表
} AppCleanStatNode;

// --- 新增：哈希表结构 ---
typedef struct {
    AppCleanStatNode **buckets;
    int size;
} AppCleanHashTable;

// --- 新增：全局哈希表实例 ---
AppCleanHashTable *app_stats_table = NULL;


// --- 结构体 ---
typedef struct {
    char path[PATH_MAX];
    regex_t regex;
    int regex_valid;
} SpecialRule;

// --- 缓存变量 ---
static char **cached_blacklist1 = NULL;
static int cached_bl1_count = 0;
static time_t last_mtime_bl1 = 0;

static char **cached_blacklist2 = NULL;
static int cached_bl2_count = 0;
static time_t last_mtime_bl2 = 0;

static char **cached_whitelist = NULL;
static int cached_wl_count = 0;
static time_t last_mtime_wl = 0;

// --- 前向声明 ---
void log_message(int level, const char *format, ...);
int ensure_log_file_is_open();
void delete_directory_recursive(const char *path, char **whitelist, int wl_count,
    regex_t *optional_regex, int check_expiry, int days, int skip_root);
static void process_blacklist(char **blacklist, int count, char **whitelist, int wl_count,
                              int check_expiry, int days);
int is_in_whitelist(const char *path, char **whitelist, int whitelist_count);
int is_expired(const char *path, int days);
static void delete_item(const char *path, int is_dir);
static int filename_matches_regex(const char *filename, regex_t *regex);

// --- 新增：哈希表和应用清理功能的前向声明 ---
static void initialize_app_stats();
static void reset_app_stats();
static void cleanup_app_stats();
static void update_app_clean_stats(const char *path, long long size);
static void log_app_cleanup_summary();


// --- 函数定义 ---

// --- 新增：哈希函数 (djb2算法，简单高效) ---
static unsigned long hash_function(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

// --- 新增：创建并初始化哈希表 ---
static void initialize_app_stats() {
    if (app_stats_table != NULL) {
        // 如果已存在，先重置
        reset_app_stats();
        return;
    }
    app_stats_table = (AppCleanHashTable*)malloc(sizeof(AppCleanHashTable));
    if (!app_stats_table) {
        log_message(0, "致命错误: 无法为应用统计哈希表分配内存。");
        exit(EXIT_FAILURE);
    }
    app_stats_table->size = HASH_TABLE_SIZE;
    app_stats_table->buckets = (AppCleanStatNode**)calloc(app_stats_table->size, sizeof(AppCleanStatNode*));
    if (!app_stats_table->buckets) {
        log_message(0, "致命错误: 无法为应用统计哈希表桶分配内存。");
        free(app_stats_table);
        app_stats_table = NULL;
        exit(EXIT_FAILURE);
    }
}

// --- 新增：清空哈希表内容，用于新一轮循环 ---
static void reset_app_stats() {
    if (!app_stats_table) {
        initialize_app_stats();
        return;
    }

    for (int i = 0; i < app_stats_table->size; i++) {
        AppCleanStatNode *current = app_stats_table->buckets[i];
        while (current != NULL) {
            AppCleanStatNode *temp = current;
            current = current->next;
            free(temp->packageName);
            free(temp);
        }
        app_stats_table->buckets[i] = NULL;
    }
}

// --- 新增：程序退出时完全释放哈希表资源 ---
static void cleanup_app_stats() {
    if (!app_stats_table) return;
    
    // 先重置（释放所有节点和包名）
    reset_app_stats();

    // 再释放桶数组和哈希表结构本身
    free(app_stats_table->buckets);
    free(app_stats_table);
    app_stats_table = NULL;
}

// --- 新增：从路径中提取包名并更新统计 ---
static void update_app_clean_stats(const char *path, long long size) {
    // 检查路径是否以指定前缀开头
    if (strncmp(path, APP_CLEAN_PREFIX, strlen(APP_CLEAN_PREFIX)) != 0) {
        return;
    }

    // 提取包名
    const char *start = path + strlen(APP_CLEAN_PREFIX);
    const char *end = strchr(start, '/');
    if (!end || start == end) {
        // 路径格式不正确，如 /data/user/0/ 或 /data/user/0/file
        return;
    }

    size_t pkg_len = end - start;
    char package_name[256]; // 假设包名不会超过255个字符
    if (pkg_len >= sizeof(package_name)) {
        log_message(1, "警告: 检测到过长的应用包名，跳过统计: %.*s", (int)pkg_len, start);
        return;
    }
    strncpy(package_name, start, pkg_len);
    package_name[pkg_len] = '\0';

    // 更新哈希表
    unsigned long index = hash_function(package_name) % app_stats_table->size;
    AppCleanStatNode *node = app_stats_table->buckets[index];

    // 查找是否已存在该包名的节点
    while (node != NULL) {
        if (strcmp(node->packageName, package_name) == 0) {
            node->totalBytes += size;
            return;
        }
        node = node->next;
    }

    // 如果不存在，创建新节点并插入到链表头部
    AppCleanStatNode *new_node = (AppCleanStatNode*)malloc(sizeof(AppCleanStatNode));
    if (!new_node) {
        log_message(1, "警告: 无法为应用统计节点分配内存，跳过: %s", package_name);
        return;
    }
    new_node->packageName = strdup(package_name);
    if (!new_node->packageName) {
        log_message(1, "警告: 无法为应用包名分配内存，跳过: %s", package_name);
        free(new_node);
        return;
    }
    new_node->totalBytes = size;
    new_node->next = app_stats_table->buckets[index];
    app_stats_table->buckets[index] = new_node;
}

// --- 新增：在循环结束时，将应用清理统计写入 app-clean.log ---
static void log_app_cleanup_summary() {
    if (!app_stats_table) return;

    app_log_file = fopen("app-clean.log", "w");
    if (!app_log_file) {
        log_message(1, "错误: 无法打开或创建 app-clean.log 文件进行写入: %s", strerror(errno));
        return;
    }

    time_t now = time(NULL);
    char time_buf[30];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(app_log_file, "--- 应用垃圾清理统计 [%s] ---\n", time_buf);
    
    int count = 0;
    for (int i = 0; i < app_stats_table->size; i++) {
        AppCleanStatNode *node = app_stats_table->buckets[i];
        while (node != NULL) {
            double deleted_mb = (node->totalBytes > 0) ? (double)node->totalBytes / (1024.0 * 1024.0) : 0.0;
            fprintf(app_log_file, "包名: %-40s | 已清理: %10.2f MB (%lld Bytes)\n", node->packageName, deleted_mb, node->totalBytes);
            node = node->next;
            count++;
        }
    }

    if (count == 0) {
        fprintf(app_log_file, "本次运行未清理任何符合条件的应用垃圾。\n");
    }

    fprintf(app_log_file, "--- 统计结束 ---\n");
    fclose(app_log_file);
    app_log_file = NULL; // 关闭后重置句柄
}


long long get_file_size(const char *filename) {
    struct stat statbuf;
    return (lstat(filename, &statbuf) == 0) ? statbuf.st_size : -1;
}

time_t get_file_mtime(const char *filename) {
    struct stat statbuf;
    if (lstat(filename, &statbuf) == 0) {
        return statbuf.st_mtime;
    }
    fprintf(stderr, "无法获取文件 '%s' 的状态信息: %s\n", filename, strerror(errno));
    log_message(1, "无法获取文件 '%s' 的状态信息: %s", filename, strerror(errno));
    return (time_t)0;
}

int ensure_log_file_is_open() {
    FILE* temp_log_file;
    struct stat statbuf;
    time_t now;
    char time_buf[30];
    int need_reopen = 0;

    if (log_file == NULL) {
        need_reopen = 1;
    } else {
        int fd = fileno(log_file);
        if (fstat(fd, &statbuf) != 0) {
            if (log_file) {
                fclose(log_file);
                log_file = NULL;
            }
            need_reopen = 1;
        } else {
            if (access("run.log", F_OK) != 0 || access("run.log", W_OK) != 0) {
                if (log_file) {
                    fclose(log_file);
                    log_file = NULL;
                }
                need_reopen = 1;
            }
        }
    }

    if (need_reopen) {
        if (log_file != NULL) {
            fclose(log_file);
            log_file = NULL;
        }

        const char* modes[] = {"a", "a+", "w"};
        int opened = 0;

        for (int i = 0; i < 3 && !opened; i++) {
            temp_log_file = fopen("run.log", modes[i]);
            if (temp_log_file != NULL) {
                if (fprintf(temp_log_file, "") >= 0 && fflush(temp_log_file) == 0) {
                    opened = 1;
                    break;
                }
                fclose(temp_log_file);
            }
        }

        if (!opened) {
            return 0;
        }

        setvbuf(temp_log_file, NULL, _IOLBF, BUFSIZ);
        log_file = temp_log_file;

        now = time(NULL);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        
        if (fprintf(log_file, "[%s] 日志文件已重新打开\n", time_buf) < 0 || 
            fflush(log_file) != 0) {
            fclose(log_file);
            log_file = NULL;
            return 0;
        }
    }

    return (log_file != NULL);
}

void rotate_log_file() {
    FILE* old_log_file = log_file;
    log_file = NULL;

    if (old_log_file != NULL) {
        if (fclose(old_log_file) != 0) {
            perror("警告: 关闭旧日志文件失败");
            fprintf(stderr, "警告: 关闭旧日志文件失败: %s\n", strerror(errno));
        }
    } else {
         fprintf(stderr, "警告: 尝试滚动日志时发现日志文件句柄已为 NULL。\n");
    }


    if (rename("run.log", "run.log.old") == 0) {
        fprintf(stderr, "日志文件已滚动为 run.log.old\n");
        log_message(1, "日志文件已滚动为 run.log.old");
    } else {
        fprintf(stderr, "日志文件重命名失败: %s\n", strerror(errno));
        log_message(1, "日志文件重命名失败: %s", strerror(errno));
    }

    ensure_log_file_is_open();
}

void check_and_rotate_log() {
    if (!ensure_log_file_is_open()) {
        return;
    }

    long long current_size = get_file_size("run.log");

    if (current_size == -1) {
        if (errno != ENOENT) {
             log_message(1, "检查日志大小时无法获取 'run.log' 状态: %s", strerror(errno));
        }
    } else if (current_size > MAX_LOG_SIZE) {
        log_message(1, "日志文件大小 (%lld bytes) 超出限制 (%d bytes)，开始滚动...", current_size, MAX_LOG_SIZE);
        rotate_log_file();
    }
}

void log_message(int level, const char *format, ...) {
    if (debug_level < level)
        return;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!ensure_log_file_is_open()) {
            if (attempt == 2) {
                fprintf(stderr, "错误: 无法重新打开日志文件，消息丢失: ");
                va_list args;
                va_start(args, format);
                vfprintf(stderr, format, args);
                fprintf(stderr, "\n");
                va_end(args);
                return;
            }
            sleep(1);
            continue;
        }

        va_list args;
        va_start(args, format);
        int write_result = vfprintf(log_file, format, args);
        va_end(args);
        
        if (write_result >= 0) {
            write_result = fprintf(log_file, "\n");
        }

        if (write_result >= 0 && fflush(log_file) == 0) {
            return;
        }

        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }

        if (attempt == 2) {
            fprintf(stderr, "错误: 写入日志失败，消息丢失: ");
            va_list args;
            va_start(args, format);
            vfprintf(stderr, format, args);
            fprintf(stderr, "\n");
            va_end(args);
        }
    }
}

// --- 修改：delete_item 函数 ---
// 在删除文件后，调用 update_app_clean_stats 进行统计
static void delete_item(const char *path, int is_dir) {
    if (!path)
        return;

    struct stat statbuf_before;
    long long item_size = 0;
    int is_link_before = 0;

    if (lstat(path, &statbuf_before) == 0) {
        is_link_before = S_ISLNK(statbuf_before.st_mode);
        if (!is_dir) {
            item_size = statbuf_before.st_size;
        }
    } else {
        if (errno != ENOENT) {
            log_message(1, "警告: 删除前无法获取 '%s' 的状态: %s", path, strerror(errno));
        }
    }

    int (*remove_func)(const char *) = is_dir ? rmdir : remove;

    if (remove_func(path) == 0) {
        if (is_dir) {
            total_dirs_deleted++;
            log_message(2, "已删除目录: %s", path);
        } else {
            total_files_deleted++;
            total_bytes_deleted_this_cycle += item_size;
            
            // --- 新增：调用应用清理统计函数 ---
            // 这一步确保了删除的字节既计入全局统计，也计入应用独立统计
            if (app_stats_table) {
                update_app_clean_stats(path, item_size);
            }
            
            log_message(2, "已删除%s: %s (大小: %lld bytes)", is_link_before ? "符号链接" : "文件", path, item_size);
        }
    } else {
        if (errno == ENOENT) {
             log_message(2, "尝试删除%s '%s' 时失败: 文件或目录不存在", is_dir ? "目录" : "文件/链接", path);
        } else if (errno == EACCES) {
             log_message(1, "删除%s '%s' 失败: 权限不足", is_dir ? "目录" : "文件/链接", path);
        } else if (errno == ENOTEMPTY && is_dir) {
             log_message(2, "删除目录 '%s' 失败: 目录非空 (可能包含未删除或白名单项)", path);
        }
         else {
            log_message(1, "删除%s '%s' 失败: %s (错误码: %d)", is_dir ? "目录" : "文件/链接", path, strerror(errno), errno);
        }
    }
}

// ... (wildcard_to_regex, parse_special_rule, free_special_rule, filename_matches_regex, is_in_whitelist, read_file_to_array, is_expired, delete_directory_recursive, process_recursive_double_star, process_blacklist, free_array 等函数保持不变)
char *wildcard_to_regex(const char *wildcard) {
    if (!wildcard)
        return NULL;
    size_t len = strlen(wildcard);
    char *regex_str = malloc(len * 2 + 3);
    if (!regex_str) {
        log_message(0, "内存分配失败 (wildcard_to_regex)");
        return NULL;
    }
    char *p = regex_str;
    *p++ = '^';
    for (size_t i = 0; i < len; i++) {
        switch (wildcard[i]) {
            case '*': *p++ = '.'; *p++ = '*'; break;
            case '?': *p++ = '.'; break;
            case '.': case '^': case '$': case '+': case '|':
            case '(': case ')': case '{': case '}': case '\\':
            case '[': case ']':
                 *p++ = '\\'; *p++ = wildcard[i]; break;
            default: *p++ = wildcard[i];
        }
    }
    *p++ = '$';
    *p = '\0';
    return regex_str;
}

static int parse_special_rule(const char *rule_str, SpecialRule *rule) {
    if (!rule_str || !rule)
        return 0;
    memset(rule, 0, sizeof(SpecialRule));
    const char *pattern_start = strrchr(rule_str, '[');
    const char *pattern_end = pattern_start ? strrchr(pattern_start, ']') : NULL;

    if (!pattern_start || !pattern_end || pattern_end <= pattern_start) {
        return 0;
    }
    if ((pattern_start > rule_str && *(pattern_start - 1) == '\\') ||
        (pattern_end > rule_str && *(pattern_end - 1) == '\\')) {
         return 0;
    }

    size_t path_len = pattern_start - rule_str;
    while (path_len > 0 && rule_str[path_len - 1] == '/') {
        path_len--;
    }

    if (path_len >= PATH_MAX) {
        log_message(1, "规则错误: 特殊规则中的路径过长: %s", rule_str);
        return 0;
    }
    strncpy(rule->path, rule_str, path_len);
    rule->path[path_len] = '\0';
    if (path_len == 0 && rule_str[0] == '/') {
         strcpy(rule->path, "/");
    } else if (path_len == 0) {
         strcpy(rule->path, ".");
    }

    size_t pattern_len = pattern_end - (pattern_start + 1);
    if (pattern_len == 0) {
        log_message(1, "规则错误: 特殊规则中的模式为空: %s", rule_str);
        return 0;
    }
    char *wildcard_pattern = malloc(pattern_len + 1);
    if (!wildcard_pattern) {
        log_message(0, "内存分配失败 (parse_special_rule:wildcard_pattern)");
        return 0;
    }
    strncpy(wildcard_pattern, pattern_start + 1, pattern_len);
    wildcard_pattern[pattern_len] = '\0';

    char *final_regex = wildcard_to_regex(wildcard_pattern);
    free(wildcard_pattern);

    if (!final_regex) {
        log_message(1, "规则错误: 通配符转换为正则表达式失败 (规则: %s)", rule_str);
        return 0;
    }

    int reg_flags = REG_EXTENDED | REG_NOSUB | REG_NEWLINE;
    int ret = regcomp(&rule->regex, final_regex, reg_flags);
    if (ret != 0) {
        char errbuf[256];
        regerror(ret, &rule->regex, errbuf, sizeof(errbuf));
        log_message(1, "规则错误: 正则表达式编译失败: '%s' (来自规则 '%s'), 错误: %s", final_regex, rule_str, errbuf);
        free(final_regex);
        return 0;
    }

    free(final_regex);
    rule->regex_valid = 1;
    log_message(2, "成功解析特殊规则: 路径='%s', 模式已编译 (来自规则 '%s')", rule->path, rule_str);
    return 1;
}

void free_special_rule(SpecialRule *rule) {
    if (rule && rule->regex_valid) {
        regfree(&rule->regex);
        rule->regex_valid = 0;
    }
}

static int filename_matches_regex(const char *filename, regex_t *regex) {
    if (!filename || !regex) {
        return 0;
    }
    return (regexec(regex, filename, 0, NULL, 0) == 0);
}

int is_in_whitelist(const char *path, char **whitelist, int whitelist_count) {
    if (!path || !whitelist || whitelist_count <= 0)
        return 0;

    char absolute_path[PATH_MAX];
    if (realpath(path, absolute_path) == NULL) {
        strncpy(absolute_path, path, PATH_MAX -1);
        absolute_path[PATH_MAX - 1] = '\0';
    }

    for (int i = 0; i < whitelist_count; i++) {
        if (!whitelist[i] || whitelist[i][0] == '\0') continue;

        char absolute_whitelist_entry[PATH_MAX];
         if (realpath(whitelist[i], absolute_whitelist_entry) == NULL) {
             strncpy(absolute_whitelist_entry, whitelist[i], PATH_MAX -1);
             absolute_whitelist_entry[PATH_MAX - 1] = '\0';
         }

        if (fnmatch(absolute_whitelist_entry, absolute_path, FNM_PATHNAME | FNM_LEADING_DIR) == 0) {
            log_message(2, "路径 '%s' (绝对路径 '%s') 匹配白名单规则 '%s' (绝对路径 '%s') [fnmatch]", path, absolute_path, whitelist[i], absolute_whitelist_entry);
            return 1;
        }

        size_t wl_len = strlen(absolute_whitelist_entry);
        if (wl_len > 0 && strncmp(absolute_path, absolute_whitelist_entry, wl_len) == 0) {
            if (absolute_path[wl_len] == '/' || absolute_path[wl_len] == '\0' || absolute_whitelist_entry[wl_len-1] == '/') {
                 log_message(2, "路径 '%s' (绝对路径 '%s') 是白名单目录 '%s' (绝对路径 '%s') 的子项", path, absolute_path, whitelist[i], absolute_whitelist_entry);
                 return 1;
            }
        }
    }

    return 0;
}

int read_file_to_array(const char *filename, char ***array) {
    if (!filename || !array)
        return 0;
    *array = NULL;

    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "错误: 无法打开配置文件: %s, 原因: %s\n", filename, strerror(errno));
        log_message(0, "无法打开配置文件: %s, 原因: %s", filename, strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t read;
    int count = 0;
    int capacity = 10;

    char **temp_array = malloc(sizeof(char*) * capacity);
    if (!temp_array) {
        log_message(0, "内存分配失败 (read_file_to_array: initial temp_array)");
        fclose(file);
        free(line);
        return -1;
    }

    while ((read = getline(&line, &line_cap, file)) != -1) {
        char *start = line;
        while (*start && (*start == ' ' || *start == '\t')) {
            start++;
        }
        char *end = start + strlen(start) - 1;
        while (end >= start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
            *end = '\0';
            end--;
        }
        if (*start == '#' || *start == '\0') {
            continue;
        }

        if (count >= capacity) {
            capacity *= 2;
            char **new_array = realloc(temp_array, sizeof(char*) * capacity);
            if (!new_array) {
                log_message(0, "内存重分配失败 (read_file_to_array: realloc)");
                for (int j = 0; j < count; j++) free(temp_array[j]);
                free(temp_array);
                free(line);
                fclose(file);
                return -1;
            }
            temp_array = new_array;
        }

        temp_array[count] = strdup(start);
        if (!temp_array[count]) {
            log_message(0, "内存分配失败 (read_file_to_array: strdup)");
            for (int j = 0; j < count; j++) free(temp_array[j]);
            free(temp_array);
            free(line);
            fclose(file);
            return -1;
        }
        count++;
    }

    free(line);
    fclose(file);

    if (count == 0) {
        free(temp_array);
        temp_array = NULL;
        log_message(2, "配置文件 '%s' 为空或只包含注释/空行。", filename);
    } else {
         char **final_array = realloc(temp_array, sizeof(char*) * count);
         if (final_array) {
             temp_array = final_array;
         }
    }

    *array = temp_array;
    return count;
}

int is_expired(const char *path, int days) {
    if (!path || days < 0)
        return 0;

    struct stat statbuf;
    if (lstat(path, &statbuf) == 0) {
        time_t current_time = time(NULL);
        if (current_time == (time_t)-1) {
            log_message(1, "警告: 获取当前时间失败，无法检查 '%s' 的过期状态", path);
            return 0;
        }
        double diff_seconds = difftime(current_time, statbuf.st_mtime);
        double expiry_seconds = (double)days * 24.0 * 3600.0;
        return (diff_seconds > expiry_seconds);
    } else {
        if (errno != ENOENT) {
             log_message(1, "警告: 无法获取 '%s' 的状态信息以检查过期: %s", path, strerror(errno));
        }
        return 0;
    }
}

void delete_directory_recursive(const char *path, char **whitelist, int wl_count,
    regex_t *optional_regex, int check_expiry, int days, int skip_root)
{
    if (!path) return;

    if (!skip_root && is_in_whitelist(path, whitelist, wl_count)) {
        log_message(2, "目录 '%s' 在白名单中，跳过删除。", path);
        return;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        if (errno == ENOTDIR) {
             log_message(2, "'%s' 不是一个目录，无法递归删除。", path);
        } else if (errno == ENOENT) {
             log_message(2, "目录 '%s' 不存在，无法递归删除。", path);
        } else if (errno == EACCES) {
             log_message(1, "无法打开目录 '%s' 进行递归删除: 权限不足", path);
        } else {
            log_message(1, "无法打开目录 '%s' 进行递归删除: %s", path, strerror(errno));
        }
        return;
    }

    log_message(2, "开始递归处理目录: %s", path);
    struct dirent *entry;
    char full_path[PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int len = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (len < 0 || (size_t)len >= sizeof(full_path)) {
            log_message(1, "路径过长，跳过: %s/%s", path, entry->d_name);
            continue;
        }

        if (is_in_whitelist(full_path, whitelist, wl_count)) {
            log_message(2, "项目 '%s' 在白名单中，跳过。", full_path);
            continue;
        }

        struct stat statbuf;
        if (lstat(full_path, &statbuf) != 0) {
            if (errno == ENOENT) {
                 log_message(2, "项目 '%s' 在处理期间消失，跳过。", full_path);
            } else {
                 log_message(1, "警告: 无法获取 '%s' 的状态信息: %s", full_path, strerror(errno));
            }
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            delete_directory_recursive(full_path, whitelist, wl_count, optional_regex, check_expiry, days, 0);
        } else {
            const char *filename = entry->d_name;
            int matches = (!optional_regex || filename_matches_regex(filename, optional_regex));
            int expired = (!check_expiry || is_expired(full_path, days));

            if (matches && expired) {
                log_message(2, "准备删除文件/链接: %s (匹配规则: %d, 过期: %d)", full_path, matches, expired);
                delete_item(full_path, 0);
            } else {
                 log_message(2, "跳过文件/链接: %s (匹配规则: %d, 过期: %d)", full_path, matches, expired);
            }
        }
    }

    closedir(dir);

    if (!skip_root) {
        log_message(2, "尝试删除目录 (如果为空): %s", path);
        delete_item(path, 1);
    }
}

static void process_recursive_double_star(const char *current_path,
                                          const char *pattern_to_match,
                                          char **whitelist, int wl_count,
                                          int check_expiry, int days,
                                          const char* original_rule)
{
    DIR *dir = opendir(current_path);
    if (!dir) {
        if (errno != ENOENT && errno != EACCES && errno != ENOTDIR) {
             log_message(1, "警告: 无法打开目录 '%s' (递归 '**' 来自规则 '%s'): %s", current_path, original_rule, strerror(errno));
        }
        return;
    }

    struct dirent *entry;
    char full_path[PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        int len_snprintf = snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);
        if (len_snprintf < 0 || (size_t)len_snprintf >= sizeof(full_path)) {
            log_message(1, "路径过长，跳过 (递归 '**' 来自规则 '%s'): %s/%s", original_rule, current_path, entry->d_name);
            continue;
        }

        if (is_in_whitelist(full_path, whitelist, wl_count)) {
            log_message(2, "项目 '%s' (递归 '**' 来自规则 '%s') 在白名单中，跳过。", full_path, original_rule);
            continue;
        }

        struct stat st;
        if (lstat(full_path, &st) != 0) {
            if (errno != ENOENT) {
                 log_message(1, "警告: 无法获取状态信息 '%s' (递归 '**' 来自规则 '%s'): %s", full_path, original_rule, strerror(errno));
            }
            continue;
        }

        int matches_pattern = 0;
        if (pattern_to_match == NULL) {
            matches_pattern = 1;
        } else {
            if (fnmatch(pattern_to_match, entry->d_name, FNM_PATHNAME) == 0) {
                matches_pattern = 1;
            }
        }

        if (S_ISDIR(st.st_mode)) {
            if (matches_pattern) {
                log_message(2, "递归 '**' 匹配到目录 '%s' (模式 '%s', 来自规则 '%s')，将递归删除。", full_path, pattern_to_match ? pattern_to_match : "*", original_rule);
                delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                continue;
            }
            process_recursive_double_star(full_path, pattern_to_match, whitelist, wl_count, check_expiry, days, original_rule);

        } else {
            if (matches_pattern) {
                if (!check_expiry || is_expired(full_path, days)) {
                    log_message(2, "准备删除 (递归 '**' 匹配 '%s', 来自规则 '%s'): %s", pattern_to_match ? pattern_to_match : "*", original_rule, full_path);
                    delete_item(full_path, 0);
                } else {
                     log_message(2, "跳过 (递归 '**' 匹配 '%s', 未过期, 来自规则 '%s'): %s", pattern_to_match ? pattern_to_match : "*", original_rule, full_path);
                }
            }
        }
    }
    closedir(dir);
}


static void process_blacklist(char **blacklist, int count, char **whitelist, int wl_count,
                              int check_expiry, int days)
{
    if (!blacklist || count <= 0) return;

    log_message(1, "开始处理黑名单规则 (共 %d 条, 过期检查: %s, 天数: %d)", count, check_expiry ? "是" : "否", days);

    for (int i = 0; i < count; i++) {
        if (!blacklist[i] || blacklist[i][0] == '\0') continue;

        char *rule = blacklist[i];
        log_message(2, "处理规则 #%d: %s", i + 1, rule);

        SpecialRule special_rule;
        if (parse_special_rule(rule, &special_rule)) {
            if (is_in_whitelist(special_rule.path, whitelist, wl_count)) {
                log_message(2, "特殊规则路径 '%s' (来自规则 '%s') 在白名单中，跳过规则。", special_rule.path, rule);
                free_special_rule(&special_rule);
                continue;
            }

            DIR *dir = opendir(special_rule.path);
            if (!dir) {
                if (errno != ENOENT) {
                    log_message(1, "规则错误: 无法打开特殊规则路径 '%s' (来自规则 '%s'): %s", special_rule.path, rule, strerror(errno));
                } else {
                    log_message(2, "特殊规则路径 '%s' (来自规则 '%s') 不存在，跳过。", special_rule.path, rule);
                }
                free_special_rule(&special_rule);
                continue;
            }

            struct dirent *entry;
            char full_path[PATH_MAX];
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

                if (filename_matches_regex(entry->d_name, &special_rule.regex)) {
                    int len = snprintf(full_path, sizeof(full_path), "%s/%s", special_rule.path, entry->d_name);
                    if (len < 0 || (size_t)len >= sizeof(full_path)) {
                        log_message(1, "路径过长，跳过 (来自规则 '%s'): %s/%s", rule, special_rule.path, entry->d_name);
                        continue;
                    }

                    if (is_in_whitelist(full_path, whitelist, wl_count)) {
                        log_message(2, "项目 '%s' (匹配特殊规则 '%s') 在白名单中，跳过。", full_path, rule);
                        continue;
                    }

                    struct stat st;
                    if (lstat(full_path, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            log_message(2, "特殊规则 '%s' 匹配到目录 '%s'，将递归删除。", rule, full_path);
                            delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                        } else {
                            if (!check_expiry || is_expired(full_path, days)) {
                                log_message(2, "准备删除 (特殊规则 '%s' 匹配): %s", rule, full_path);
                                delete_item(full_path, 0);
                            } else {
                                 log_message(2, "跳过 (特殊规则 '%s' 匹配，未过期): %s", rule, full_path);
                            }
                        }
                    } else if (errno != ENOENT) {
                        log_message(1, "警告: 无法获取状态信息 '%s' (来自规则 '%s'): %s", full_path, rule, strerror(errno));
                    }
                }
            }
            closedir(dir);
            free_special_rule(&special_rule);
            continue;
        }

        char target_path_normalized[PATH_MAX];
        strncpy(target_path_normalized, rule, PATH_MAX -1);
        target_path_normalized[PATH_MAX - 1] = '\0';
        size_t len = strlen(target_path_normalized);
        while (len > 1 && target_path_normalized[len - 1] == '/') {
            target_path_normalized[len - 1] = '\0';
            len--;
        }

        if (is_in_whitelist(target_path_normalized, whitelist, wl_count)) {
            log_message(2, "规则目标 '%s' (标准化: '%s') 在白名单中，跳过。", rule, target_path_normalized);
            continue;
        }

        int has_wildcard = (strpbrk(target_path_normalized, "*?[]") != NULL);
        char *recursive_marker = strstr(target_path_normalized, "**");

        if (recursive_marker) {
            log_message(2, "检测到递归通配符 '**' 在规则中: %s", rule);

            char base_path[PATH_MAX];
            char *pattern = NULL;

            size_t base_len = recursive_marker - target_path_normalized;
            while (base_len > 0 && target_path_normalized[base_len - 1] == '/') {
                base_len--;
            }
            if (base_len == 0 && target_path_normalized[0] == '/') {
                 strcpy(base_path, "/");
            } else if (base_len == 0) {
                strcpy(base_path, ".");
            } else {
                strncpy(base_path, target_path_normalized, base_len);
                base_path[base_len] = '\0';
            }

            char *pattern_start = recursive_marker + 2;
            while (*pattern_start == '/') {
                pattern_start++;
            }
            if (*pattern_start != '\0') {
                pattern = pattern_start;
            } else {
                 pattern = NULL;
            }

            log_message(2, "递归处理: 基础路径='%s', 模式='%s' (来自规则 '%s')", base_path, pattern ? pattern : "(无)", rule);
            process_recursive_double_star(base_path, pattern, whitelist, wl_count, check_expiry, days, rule);

        } else if (has_wildcard) {
            log_message(2, "检测到非递归通配符在规则中: %s", rule);
            char base_path[PATH_MAX];
            char *pattern;

            char *last_slash = strrchr(target_path_normalized, '/');
            if (last_slash) {
                pattern = last_slash + 1;
                size_t base_len = last_slash - target_path_normalized;
                if (base_len == 0) {
                    strcpy(base_path, "/");
                } else {
                    strncpy(base_path, target_path_normalized, base_len);
                    base_path[base_len] = '\0';
                }
            } else {
                pattern = target_path_normalized;
                strcpy(base_path, ".");
            }

            if (!pattern || *pattern == '\0') {
                 log_message(1, "规则错误: 从规则 '%s' 中提取通配符模式失败。", rule);
                 continue;
            }

            log_message(2, "通配符处理: 基础路径='%s', 模式='%s' (来自规则 '%s')", base_path, pattern, rule);

            DIR *dir = opendir(base_path);
            if (!dir) {
                if (errno != ENOENT) {
                    log_message(1, "规则错误: 无法打开基础路径 '%s' (来自规则 '%s'): %s", base_path, rule, strerror(errno));
                } else {
                     log_message(2, "基础路径 '%s' (来自规则 '%s') 不存在，跳过。", base_path, rule);
                }
                continue;
            }

            struct dirent *entry;
            char full_path[PATH_MAX];
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

                if (fnmatch(pattern, entry->d_name, FNM_PATHNAME) == 0) {
                    int len_snprintf = snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
                     if (len_snprintf < 0 || (size_t)len_snprintf >= sizeof(full_path)) {
                        log_message(1, "路径过长，跳过 (来自规则 '%s'): %s/%s", rule, base_path, entry->d_name);
                        continue;
                    }

                    if (is_in_whitelist(full_path, whitelist, wl_count)) {
                        log_message(2, "项目 '%s' (匹配通配符 '%s', 来自规则 '%s') 在白名单中，跳过。", full_path, pattern, rule);
                        continue;
                    }

                    struct stat st;
                    if (lstat(full_path, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            log_message(2, "通配符 '%s' (来自规则 '%s') 匹配到目录 '%s'，将递归删除。", pattern, rule, full_path);
                            delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                        } else {
                            if (!check_expiry || is_expired(full_path, days)) {
                                log_message(2, "准备删除 (通配符 '%s' 匹配, 来自规则 '%s'): %s", pattern, rule, full_path);
                                delete_item(full_path, 0);
                            } else {
                                 log_message(2, "跳过 (通配符 '%s' 匹配，未过期, 来自规则 '%s'): %s", pattern, rule, full_path);
                            }
                        }
                    } else if (errno != ENOENT) {
                        log_message(1, "警告: 无法获取状态信息 '%s' (来自规则 '%s'): %s", full_path, rule, strerror(errno));
                    }
                }
            }
            closedir(dir);

        } else {
            log_message(2, "处理精确路径规则: %s", rule);

            struct stat st;
            if (lstat(target_path_normalized, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    log_message(2, "精确路径 '%s' (来自规则 '%s') 是目录，将递归删除。", target_path_normalized, rule);
                    delete_directory_recursive(target_path_normalized, whitelist, wl_count, NULL, check_expiry, days, 0);
                } else {
                    if (!check_expiry || is_expired(target_path_normalized, days)) {
                        log_message(2, "准备删除精确路径 (文件/链接, 来自规则 '%s'): %s", rule, target_path_normalized);
                        delete_item(target_path_normalized, 0);
                    } else {
                         log_message(2, "跳过精确路径 (文件/链接，未过期, 来自规则 '%s'): %s", rule, target_path_normalized);
                    }
                }
            } else {
                if (errno == ENOENT) {
                    log_message(2, "精确路径 '%s' (来自规则 '%s') 未找到。", target_path_normalized, rule);
                } else {
                    log_message(1, "警告: 无法获取精确路径 '%s' (来自规则 '%s') 的状态信息: %s", target_path_normalized, rule, strerror(errno));
                }
            }
        }
    }
     log_message(2, "黑名单规则处理完成。");
}

void free_array(char **array, int count) {
    if (!array) return;
    for (int i = 0; i < count; i++) {
        free(array[i]);
        array[i] = NULL;
    }
    free(array);
}


// --- 修改：main 函数 ---
// 添加了哈希表的初始化、重置和清理，以及在循环结束时调用新的日志函数
int main(int argc, char *argv[]) {
    strncpy(program_name, argv[0], sizeof(program_name) - 1);
    program_name[sizeof(program_name) - 1] = '\0';

    static struct option long_options[] = {
        {"blacklist1", required_argument, 0, '1'},
        {"blacklist2", required_argument, 0, '2'},
        {"whitelist",  required_argument, 0, 'w'},
        {"days",       required_argument, 0, 'D'},
        {"seconds",    required_argument, 0, 's'},
        {"debug",      required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int opt;
    int seconds = 0;
    int days = 0;
    char *blacklist1_file = NULL;
    char *blacklist2_file = NULL;
    char *whitelist_file = NULL;
    char time_str[100];

    while ((opt = getopt_long(argc, argv, "1:2:w:D:s:d:", long_options, NULL)) != -1) {
        switch (opt) {
            case '1': blacklist1_file = optarg; break;
            case '2': blacklist2_file = optarg; break;
            case 'w': whitelist_file = optarg; break;
            case 'D':
                {
                 char *endptr_days; errno = 0; long val_days = strtol(optarg, &endptr_days, 10);
                 if (errno != 0 || *endptr_days != '\0' || val_days < 0 || val_days > INT_MAX) {
                    fprintf(stderr, "%s: 错误: 无效的天数 '%s' (必须是非负整数)。\n", program_name, optarg); return EXIT_FAILURE;
                 } days = (int)val_days;
                } break;
            case 's':
                {
                 char *endptr_secs; errno = 0; long val_secs = strtol(optarg, &endptr_secs, 10);
                 if (errno != 0 || *endptr_secs != '\0' || val_secs < 0 || val_secs > INT_MAX) {
                     fprintf(stderr, "%s: 错误: 无效的时间间隔 '%s' (必须是非负整数)。\n", program_name, optarg); return EXIT_FAILURE;
                 } seconds = (int)val_secs;
                } break;
            case 'd':
                {
                 char *endptr_dbg; errno = 0; long val_dbg = strtol(optarg, &endptr_dbg, 10);
                 if (errno != 0 || *endptr_dbg != '\0' || val_dbg < 0 || val_dbg > 2) {
                     fprintf(stderr, "%s: 错误: 无效的调试级别 '%s' (必须是 0, 1, 或 2)。\n", program_name, optarg); return EXIT_FAILURE;
                 } debug_level = (int)val_dbg;
                } break;
            case '?': return EXIT_FAILURE;
            default: abort();
        }
    }

    if (!blacklist1_file || !whitelist_file) {
        fprintf(stderr, "%s: 错误: 必须同时提供黑名单1 (-1 或 --blacklist1) 和白名单 (-w 或 --whitelist) 文件路径。\n", program_name);
        return EXIT_FAILURE;
    }
    if (blacklist2_file && days <= 0) {
        fprintf(stderr, "%s: 错误: 使用黑名单2 (-2 或 --blacklist2) 时，必须同时设置过期天数 (-D 或 --days) 且天数必须大于 0。\n", program_name);
        return EXIT_FAILURE;
    }

    if (!ensure_log_file_is_open()) {
        fprintf(stderr, "程序因无法初始化日志文件而退出。\n");
        return EXIT_FAILURE;
    }

    time_t boot_time = time(NULL);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&boot_time));
    log_message(1, "\n==================================================");
    log_message(1, "程序启动: %s (PID: %d)", time_str, getpid());

    if (ensure_log_file_is_open()) {
        fprintf(log_file, "命令行参数: ");
        for(int i=0; i<argc; ++i) {
            fprintf(log_file, "%s ", argv[i]);
        }
        fprintf(log_file, "\n");
        fflush(log_file);
    } else {
        fprintf(stderr, "警告: 无法记录命令行参数，日志文件不可用。\n");
    }


    log_message(1, "配置: 黑名单1='%s', 黑名单2='%s', 白名单='%s', 过期天数=%d, 循环间隔=%d秒, 日志级别=%d",
                blacklist1_file, blacklist2_file ? blacklist2_file : "(无)", whitelist_file, days, seconds, debug_level);

    // --- 新增：初始化应用统计哈希表 ---
    initialize_app_stats();

    do {
        time_t loop_start_time = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&loop_start_time));
        log_message(1, "\n---【循环开始】 时间: %s ---", time_str);

        total_files_deleted = 0;
        total_dirs_deleted = 0;
        total_bytes_deleted_this_cycle = 0;
        
        // --- 新增：重置应用统计数据以开始新一轮循环 ---
        reset_app_stats();

        // --- 加载/重新加载白名单 ---
        time_t current_mtime_wl = get_file_mtime(whitelist_file);
        if (current_mtime_wl == (time_t)0 && cached_whitelist == NULL) {
             log_message(0, "无法获取白名单 '%s' 的修改时间且无缓存，无法运行。程序终止。", whitelist_file);
             fprintf(stderr, "错误: 无法获取白名单 '%s' 的修改时间且无缓存，无法运行。程序终止。\n", whitelist_file);
             break;
        }
        if (cached_whitelist == NULL || (current_mtime_wl != (time_t)0 && current_mtime_wl != last_mtime_wl)) {
            log_message(1, "检测到白名单 '%s' 已更新或首次加载 (mtime: %ld)，重新加载...", whitelist_file, (long)current_mtime_wl);
            free_array(cached_whitelist, cached_wl_count);
            cached_whitelist = NULL;
            cached_wl_count = read_file_to_array(whitelist_file, &cached_whitelist);
            if (cached_wl_count >= 0) {
                 last_mtime_wl = current_mtime_wl;
                 log_message(1, "成功从 '%s' 加载 %d 条白名单规则。", whitelist_file, cached_wl_count);
            } else {
                 log_message(0, "从 '%s' 加载白名单规则失败。将尝试使用旧缓存（如果存在）。", whitelist_file);
                 fprintf(stderr, "错误: 从 '%s' 加载白名单规则失败。将尝试使用旧缓存（如果存在）。\n", whitelist_file);
                 last_mtime_wl = (time_t)0;
                 if (cached_whitelist == NULL) {
                      log_message(0, "初始白名单加载失败且无缓存，无法运行。程序终止。");
                      fprintf(stderr, "错误: 初始白名单加载失败且无缓存，无法运行。程序终止。\n");
                      break;
                 } else {
                      log_message(1, "警告: 继续使用旧的白名单缓存 (%d 条规则)。", cached_wl_count);
                 }
            }
        } else {
            log_message(2, "使用缓存的白名单 '%s' (%d 条规则, mtime: %ld)。", whitelist_file, cached_wl_count, (long)last_mtime_wl);
        }

        // --- 加载/重新加载黑名单 1 ---
        time_t current_mtime_bl1 = get_file_mtime(blacklist1_file);
         if (current_mtime_bl1 == (time_t)0 && cached_blacklist1 == NULL) {
             log_message(0, "无法获取黑名单1 '%s' 的修改时间且无缓存，无法运行。程序终止。", blacklist1_file);
             fprintf(stderr, "错误: 无法获取黑名单1 '%s' 的修改时间且无缓存，无法运行。程序终止。\n", blacklist1_file);
             break;
        }
        if (cached_blacklist1 == NULL || (current_mtime_bl1 != (time_t)0 && current_mtime_bl1 != last_mtime_bl1)) {
            log_message(1, "检测到黑名单1 '%s' 已更新或首次加载 (mtime: %ld)，重新加载...", blacklist1_file, (long)current_mtime_bl1);
            free_array(cached_blacklist1, cached_bl1_count);
            cached_blacklist1 = NULL;
            cached_bl1_count = read_file_to_array(blacklist1_file, &cached_blacklist1);
             if (cached_bl1_count >= 0) {
                 last_mtime_bl1 = current_mtime_bl1;
                 log_message(1, "成功从 '%s' 加载 %d 条黑名单1规则。", blacklist1_file, cached_bl1_count);
            } else {
                 log_message(0, "从 '%s' 加载黑名单1规则失败。将尝试使用旧缓存（如果存在）。", blacklist1_file);
                 fprintf(stderr, "错误: 从 '%s' 加载黑名单1规则失败。将尝试使用旧缓存（如果存在）。\n", blacklist1_file);
                 last_mtime_bl1 = (time_t)0;
                  if (cached_blacklist1 == NULL) {
                      log_message(0, "初始黑名单1加载失败且无缓存，无法运行。程序终止。");
                      fprintf(stderr, "错误: 初始黑名单1加载失败且无缓存，无法运行。程序终止。\n");
                      break;
                 } else {
                      log_message(1, "警告: 继续使用旧的黑名单1缓存 (%d 条规则)。", cached_bl1_count);
                 }
            }
        } else {
            log_message(2, "使用缓存的黑名单1 '%s' (%d 条规则, mtime: %ld)。", blacklist1_file, cached_bl1_count, (long)last_mtime_bl1);
        }

        // --- 加载/重新加载黑名单 2 ---
        if (blacklist2_file) {
            time_t current_mtime_bl2 = get_file_mtime(blacklist2_file);
             if (current_mtime_bl2 == (time_t)0 && cached_blacklist2 == NULL) {
                 log_message(1, "警告: 无法获取黑名单2 '%s' 的修改时间且无缓存。本次循环将跳过黑名单2的处理。", blacklist2_file);
                 free_array(cached_blacklist2, cached_bl2_count);
                 cached_blacklist2 = NULL;
                 cached_bl2_count = 0;
                 last_mtime_bl2 = (time_t)0;
            } else if (cached_blacklist2 == NULL || (current_mtime_bl2 != (time_t)0 && current_mtime_bl2 != last_mtime_bl2)) {
                log_message(1, "检测到黑名单2 '%s' 已更新或首次加载 (mtime: %ld)，重新加载...", blacklist2_file, (long)current_mtime_bl2);
                free_array(cached_blacklist2, cached_bl2_count);
                cached_blacklist2 = NULL;
                cached_bl2_count = read_file_to_array(blacklist2_file, &cached_blacklist2);
                 if (cached_bl2_count >= 0) {
                     last_mtime_bl2 = current_mtime_bl2;
                     log_message(1, "成功从 '%s' 加载 %d 条黑名单2规则。", blacklist2_file, cached_bl2_count);
                } else {
                     log_message(1, "警告: 从 '%s' 加载黑名单2规则失败。将尝试使用旧缓存（如果存在）。", blacklist2_file);
                     fprintf(stderr, "警告: 从 '%s' 加载黑名单2规则失败。将尝试使用旧缓存（如果存在）。\n", blacklist2_file);
                     last_mtime_bl2 = (time_t)0;
                     if (cached_blacklist2 == NULL) {
                          log_message(1, "警告: 无可用黑名单2规则缓存，本次循环将跳过黑名单2处理。");
                     } else {
                          log_message(1, "警告: 继续使用旧的黑名单2缓存 (%d 条规则)。", cached_bl2_count);
                     }
                }
            } else {
                log_message(2, "使用缓存的黑名单2 '%s' (%d 条规则, mtime: %ld)。", blacklist2_file, cached_bl2_count, (long)last_mtime_bl2);
            }
        } else {
             if (cached_blacklist2 != NULL) {
                  log_message(1, "黑名单2文件未指定或已移除，清除相关缓存。");
                  free_array(cached_blacklist2, cached_bl2_count);
                  cached_blacklist2 = NULL;
                  cached_bl2_count = 0;
                  last_mtime_bl2 = (time_t)0;
             }
        }

        // --- 处理黑名单 ---
        if (!cached_whitelist) {
             log_message(0, "没有有效的白名单数据，无法继续处理黑名单。跳过本轮处理。");
             fprintf(stderr, "错误：没有有效的白名单数据，无法继续处理黑名单。跳过本轮处理。\n");
        } else {
            if (cached_blacklist1 && cached_bl1_count > 0) {
                process_blacklist(cached_blacklist1, cached_bl1_count, cached_whitelist, cached_wl_count, 0, 0);
            } else {
                 log_message(1, "跳过黑名单1处理 (无有效规则加载)。");
            }

            if (blacklist2_file && cached_blacklist2 && cached_bl2_count > 0) {
                process_blacklist(cached_blacklist2, cached_bl2_count, cached_whitelist, cached_wl_count, 1, days);
            } else if (blacklist2_file) {
                 log_message(1, "跳过黑名单2处理 (无有效规则加载)。");
            }
        }

        // --- 记录本轮总结 ---
        time_t loop_end_time = time(NULL);
        double loop_duration = difftime(loop_end_time, loop_start_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&loop_end_time));

        // --- 新增：在记录全局总结之前，先记录应用清理的详细日志 ---
        log_app_cleanup_summary();

        double deleted_mb = (total_bytes_deleted_this_cycle > 0) ?
                            (double)total_bytes_deleted_this_cycle / (1024.0 * 1024.0) : 0.0;
        log_message(1, "全局统计: 已删除垃圾 %.2f MB", deleted_mb); 
        log_message(1, "全局统计: 已删除文件数: %d", total_files_deleted); 
        log_message(1, "全局统计: 已删除目录数: %d", total_dirs_deleted); 
        log_message(1, "---【循环结束】 耗时: %.2f 秒 ---", loop_duration); 

        // --- 等待或退出 ---
        if (seconds > 0) {
            log_message(1, "等待 %d 秒进入下一循环...", seconds); 
            sleep(seconds);
        } else {
            log_message(1, "单次执行完成，程序退出。");
            break;
        }

    } while (seconds > 0);

    // --- 清理工作 ---
    log_message(1, "程序正在关闭，释放资源...");

    free_array(cached_blacklist1, cached_bl1_count);
    free_array(cached_blacklist2, cached_bl2_count);
    free_array(cached_whitelist, cached_wl_count);

    // --- 新增：清理应用统计哈希表占用的内存 ---
    cleanup_app_stats();

    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }

    return EXIT_SUCCESS;
}
