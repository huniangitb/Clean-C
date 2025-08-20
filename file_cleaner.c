#define _GNU_SOURCE
#include "file_cleaner.h"
#include "logger.h"
#include "stats_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <fnmatch.h>

// 定义一个路径至少需要包含的组件数，防止删除根目录下的重要目录
#define MIN_PATH_COMPONENTS 1

typedef struct {
    char path[PATH_MAX];
    regex_t regex;
    int regex_valid;
} SpecialRule;

// 函数前向声明
static void delete_directory_recursive(const char *path, char **whitelist, int wl_count, regex_t *optional_regex, int check_expiry, int days, int skip_root);
static void process_blacklist(char **blacklist, int count, char **whitelist, int wl_count, int check_expiry, int days);

static void delete_item(const char *path, int is_dir) {
    if (!path) return;

    // 安全护栏: 防止删除过于接近根目录的目录
    if (is_dir) {
        char temp_path[PATH_MAX];
        strncpy(temp_path, path, PATH_MAX - 1);
        temp_path[PATH_MAX - 1] = '\0';

        int components = 0;
        char *p = temp_path;
        if (*p == '/') p++;
        while ((p = strchr(p, '/')) != NULL) {
            components++;
            p++;
        }
        if (components < MIN_PATH_COMPONENTS) {
            log_message(0, "安全警告: 尝试删除的目录 '%s' 过于接近根目录 (组件数 < %d)，操作已取消。", path, MIN_PATH_COMPONENTS);
            return;
        }
    }

    struct stat statbuf_before;
    long long item_size = 0;
    int is_link_before = 0;

    if (lstat(path, &statbuf_before) == 0) {
        is_link_before = S_ISLNK(statbuf_before.st_mode);
        if (!is_dir) {
            item_size = statbuf_before.st_size;
        }
    } else if (errno != ENOENT) {
        log_message(1, "警告: 删除前无法获取 '%s' 的状态: %s", path, strerror(errno));
    }

    int (*remove_func)(const char *) = is_dir ? rmdir : remove;
    if (remove_func(path) == 0) {
        stats_record_deletion(path, item_size, is_dir);
        if (is_dir) {
            log_message(2, "已删除目录: %s", path);
        } else {
            log_message(2, "已删除%s: %s (大小: %lld bytes)", is_link_before ? "符号链接" : "文件", path, item_size);
        }
    } else {
        if (errno == ENOENT) {
            log_message(2, "尝试删除%s '%s' 时失败: 文件或目录不存在", is_dir ? "目录" : "文件/链接", path);
        } else {
            log_message(1, "删除%s '%s' 失败: %s", is_dir ? "目录" : "文件/链接", path, strerror(errno));
        }
    }
}

static char *wildcard_to_regex(const char *wildcard) {
    if (!wildcard) return NULL;
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
            case '.': case '^': case '$': case '+': case '|': case '(': case ')': case '{': case '}': case '\\': case '[': case ']':
                *p++ = '\\'; *p++ = wildcard[i]; break;
            default: *p++ = wildcard[i];
        }
    }
    *p++ = '$';
    *p = '\0';
    return regex_str;
}

static int parse_special_rule(const char *rule_str, SpecialRule *rule) {
    if (!rule_str || !rule) return 0;
    memset(rule, 0, sizeof(SpecialRule));
    const char *pattern_start = strrchr(rule_str, '[');
    const char *pattern_end = pattern_start ? strrchr(pattern_start, ']') : NULL;
    if (!pattern_start || !pattern_end || pattern_end <= pattern_start) return 0;

    size_t path_len = pattern_start - rule_str;
    while (path_len > 0 && rule_str[path_len - 1] == '/') path_len--;
    if (path_len >= PATH_MAX) return 0;
    strncpy(rule->path, rule_str, path_len);
    rule->path[path_len] = '\0';
    if (path_len == 0) strcpy(rule->path, ".");

    size_t pattern_len = pattern_end - (pattern_start + 1);
    if (pattern_len == 0) return 0;
    char *wildcard_pattern = malloc(pattern_len + 1);
    if (!wildcard_pattern) return 0;
    strncpy(wildcard_pattern, pattern_start + 1, pattern_len);
    wildcard_pattern[pattern_len] = '\0';

    char *final_regex = wildcard_to_regex(wildcard_pattern);
    free(wildcard_pattern);
    if (!final_regex) return 0;

    if (regcomp(&rule->regex, final_regex, REG_EXTENDED | REG_NOSUB | REG_NEWLINE) != 0) {
        free(final_regex);
        return 0;
    }
    free(final_regex);
    rule->regex_valid = 1;
    return 1;
}

static void free_special_rule(SpecialRule *rule) {
    if (rule && rule->regex_valid) {
        regfree(&rule->regex);
        rule->regex_valid = 0;
    }
}

static int filename_matches_regex(const char *filename, regex_t *regex) {
    if (!filename || !regex) return 0;
    return (regexec(regex, filename, 0, NULL, 0) == 0);
}

static int is_in_whitelist(const char *path, char **whitelist, int whitelist_count) {
    if (!path || !whitelist || whitelist_count <= 0) return 0;
    char absolute_path[PATH_MAX];
    if (realpath(path, absolute_path) == NULL) {
        strncpy(absolute_path, path, PATH_MAX - 1);
        absolute_path[PATH_MAX - 1] = '\0';
    }

    for (int i = 0; i < whitelist_count; i++) {
        if (!whitelist[i] || whitelist[i][0] == '\0') continue;
        char absolute_whitelist_entry[PATH_MAX];
        if (realpath(whitelist[i], absolute_whitelist_entry) == NULL) {
            strncpy(absolute_whitelist_entry, whitelist[i], PATH_MAX - 1);
            absolute_whitelist_entry[PATH_MAX - 1] = '\0';
        }

        if (fnmatch(absolute_whitelist_entry, absolute_path, FNM_PATHNAME | FNM_LEADING_DIR) == 0) {
            log_message(2, "路径 '%s' 匹配白名单规则 '%s' [fnmatch]", path, whitelist[i]);
            return 1;
        }
        size_t wl_len = strlen(absolute_whitelist_entry);
        if (wl_len > 0 && strncmp(absolute_path, absolute_whitelist_entry, wl_len) == 0) {
            if (absolute_path[wl_len] == '/' || absolute_path[wl_len] == '\0' || absolute_whitelist_entry[wl_len - 1] == '/') {
                log_message(2, "路径 '%s' 是白名单目录 '%s' 的子项", path, whitelist[i]);
                return 1;
            }
        }
    }
    return 0;
}

static int is_expired(const char *path, int days) {
    if (!path || days < 0) return 0;
    struct stat statbuf;
    if (lstat(path, &statbuf) == 0) {
        time_t current_time = time(NULL);
        double diff_seconds = difftime(current_time, statbuf.st_mtime);
        double expiry_seconds = (double)days * 24.0 * 3600.0;
        return (diff_seconds > expiry_seconds);
    }
    if (errno != ENOENT) {
        log_message(1, "警告: 无法获取 '%s' 的状态信息以检查过期: %s", path, strerror(errno));
    }
    return 0;
}

static void process_recursive_double_star(const char *current_path, const char *pattern_to_match, char **whitelist, int wl_count, int check_expiry, int days, const char* original_rule) {
    DIR *dir = opendir(current_path);
    if (!dir) return;

    struct dirent *entry;
    char full_path[PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);
        if (is_in_whitelist(full_path, whitelist, wl_count)) continue;

        struct stat st;
        if (lstat(full_path, &st) != 0) continue;

        int matches_pattern = (pattern_to_match == NULL || fnmatch(pattern_to_match, entry->d_name, FNM_PATHNAME) == 0);

        if (S_ISDIR(st.st_mode)) {
            if (matches_pattern) {
                delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                continue;
            }
            process_recursive_double_star(full_path, pattern_to_match, whitelist, wl_count, check_expiry, days, original_rule);
        } else {
            if (matches_pattern && (!check_expiry || is_expired(full_path, days))) {
                delete_item(full_path, 0);
            }
        }
    }
    closedir(dir);
}

static void delete_directory_recursive(const char *path, char **whitelist, int wl_count, regex_t *optional_regex, int check_expiry, int days, int skip_root) {
    if (!path) return;
    if (!skip_root && is_in_whitelist(path, whitelist, wl_count)) {
        log_message(2, "目录 '%s' 在白名单中，跳过删除。", path);
        return;
    }
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    char full_path[PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (is_in_whitelist(full_path, whitelist, wl_count)) continue;

        struct stat statbuf;
        if (lstat(full_path, &statbuf) != 0) continue;

        if (S_ISDIR(statbuf.st_mode)) {
            delete_directory_recursive(full_path, whitelist, wl_count, optional_regex, check_expiry, days, 0);
        } else {
            int matches = (!optional_regex || filename_matches_regex(entry->d_name, optional_regex));
            int expired = (!check_expiry || is_expired(full_path, days));
            if (matches && expired) {
                delete_item(full_path, 0);
            }
        }
    }
    closedir(dir);
    if (!skip_root) {
        delete_item(path, 1);
    }
}

static void process_blacklist(char **blacklist, int count, char **whitelist, int wl_count, int check_expiry, int days) {
    if (!blacklist || count <= 0) return;
    log_message(1, "开始处理黑名单规则 (共 %d 条, 过期检查: %s, 天数: %d)", count, check_expiry ? "是" : "否", days);
    const char *redirect_from = "/storage/emulated/0/";
    const char *redirect_to = "/data/media/0/";

    for (int i = 0; i < count; i++) {
        if (!blacklist[i] || blacklist[i][0] == '\0') continue;
        char *original_rule = blacklist[i];
        char *resolved_rule = NULL;
        const char *rule_to_process = original_rule;

        char *redirect_pos = strstr(original_rule, redirect_from);
        if (redirect_pos) {
            size_t new_len = strlen(original_rule) - strlen(redirect_from) + strlen(redirect_to) + 1;
            resolved_rule = malloc(new_len);
            if (resolved_rule) {
                strncpy(resolved_rule, original_rule, redirect_pos - original_rule);
                resolved_rule[redirect_pos - original_rule] = '\0';
                strcat(resolved_rule, redirect_to);
                strcat(resolved_rule, redirect_pos + strlen(redirect_from));
                rule_to_process = resolved_rule;
            }
        }
        log_message(2, "处理规则 #%d: %s (实际处理路径: %s)", i + 1, original_rule, rule_to_process);

        // 规则校验: 检查规则中是否包含 ".."
        if (strstr(rule_to_process, "..")) {
            log_message(1, "规则错误: 规则 '%s' 包含不安全的 '..' 路径，已跳过。", original_rule);
            if (resolved_rule) free(resolved_rule);
            continue;
        }

        SpecialRule special_rule;
        if (parse_special_rule(rule_to_process, &special_rule)) {
            if (!is_in_whitelist(special_rule.path, whitelist, wl_count)) {
                delete_directory_recursive(special_rule.path, whitelist, wl_count, &special_rule.regex, check_expiry, days, 1);
            }
            free_special_rule(&special_rule);
        } else {
            char target_path[PATH_MAX];
            strncpy(target_path, rule_to_process, PATH_MAX - 1);
            target_path[PATH_MAX - 1] = '\0';
            size_t len = strlen(target_path);
            if (len > 1 && target_path[len - 1] == '/') target_path[len - 1] = '\0';

            if (is_in_whitelist(target_path, whitelist, wl_count)) continue;

            char *recursive_marker = strstr(target_path, "**");
            if (recursive_marker) {
                char base_path[PATH_MAX];
                char *pattern = NULL;
                size_t base_len = recursive_marker - target_path;
                while (base_len > 0 && target_path[base_len - 1] == '/') base_len--;
                if (base_len == 0) strcpy(base_path, ".");
                else { strncpy(base_path, target_path, base_len); base_path[base_len] = '\0'; }
                
                char *pattern_start = recursive_marker + 2;
                while (*pattern_start == '/') pattern_start++;
                if (*pattern_start != '\0') pattern = pattern_start;
                
                process_recursive_double_star(base_path, pattern, whitelist, wl_count, check_expiry, days, original_rule);
            } else if (strpbrk(target_path, "*?[]")) {
                char base_path[PATH_MAX];
                char *pattern;
                char *last_slash = strrchr(target_path, '/');
                if (last_slash) {
                    pattern = last_slash + 1;
                    size_t base_len = last_slash - target_path;
                    if (base_len == 0) strcpy(base_path, "/");
                    else { strncpy(base_path, target_path, base_len); base_path[base_len] = '\0'; }
                } else {
                    pattern = target_path;
                    strcpy(base_path, ".");
                }
                
                DIR *dir = opendir(base_path);
                if (dir) {
                    struct dirent *entry;
                    char full_path[PATH_MAX];
                    while ((entry = readdir(dir)) != NULL) {
                        // --- 新增: 必须跳过 "." 和 ".." ---
                        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                            continue;
                        }
                        // --- 检查结束 ---

                        if (fnmatch(pattern, entry->d_name, FNM_PATHNAME) == 0) {
                            snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
                            if (is_in_whitelist(full_path, whitelist, wl_count)) continue;
                            struct stat st;
                            if (lstat(full_path, &st) == 0) {
                                if (S_ISDIR(st.st_mode)) {
                                    delete_directory_recursive(full_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                                } else if (!check_expiry || is_expired(full_path, days)) {
                                    delete_item(full_path, 0);
                                }
                            }
                        }
                    }
                    closedir(dir);
                }
            } else {
                struct stat st;
                if (lstat(target_path, &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        delete_directory_recursive(target_path, whitelist, wl_count, NULL, check_expiry, days, 0);
                    } else if (!check_expiry || is_expired(target_path, days)) {
                        delete_item(target_path, 0);
                    }
                }
            }
        }
        if (resolved_rule) free(resolved_rule);
    }
    log_message(2, "黑名单规则处理完成。");
}

void cleaner_process_blacklist1(const ConfigData *config) {
    if (config && config->blacklist1 && config->bl1_count > 0) {
        process_blacklist(config->blacklist1, config->bl1_count, config->whitelist, config->wl_count, 0, 0);
    } else {
        log_message(1, "跳过黑名单1处理 (无有效规则加载)。");
    }
}

void cleaner_process_blacklist2(const ConfigData *config, int days) {
    if (config && config->blacklist2 && config->bl2_count > 0) {
        process_blacklist(config->blacklist2, config->bl2_count, config->whitelist, config->wl_count, 1, days);
    } else {
        log_message(1, "跳过黑名单2处理 (无有效规则加载)。");
    }
}