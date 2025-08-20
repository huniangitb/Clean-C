// udp_server.c
#include "udp_server.h"
#include "logger.h"
#include "gc_trim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdatomic.h>

#define BUF_SIZE 1024

static pthread_t g_udp_thread;
static volatile sig_atomic_t g_udp_stop_flag = 0;
static atomic_int g_immediate_clean_request = 0;
static int g_listen_fd = -1;

// 处理接收到的命令
static void handle_command(const char *command) {
    log_message(1, "UDP: 收到命令 '%s'", command);

    if (strcmp(command, "clean_now") == 0) {
        atomic_store(&g_immediate_clean_request, 1);
        // UDP 是单向的，我们不在乎响应
    } else if (strcmp(command, "start_gc") == 0) {
        trigger_gc_once();
    } else if (strcmp(command, "stop_gc") == 0) {
        stop_current_gc();
    } else {
        log_message(1, "UDP: 收到未知命令 '%s'", command);
    }
}

// UDP 服务器主循环
static void* udp_server_thread_func(void *arg) {
    int port = *(int*)arg;
    free(arg);

    g_listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_listen_fd < 0) {
        log_message(0, "UDP: socket() 创建失败: %s", strerror(errno));
        return NULL;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        log_message(0, "UDP: bind() 到端口 %d 失败: %s", port, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return NULL;
    }

    log_message(1, "UDP 服务器已在端口 %d 上启动并等待命令...", port);

    char buffer[BUF_SIZE];
    while (!g_udp_stop_flag) {
        struct sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);

        ssize_t n = recvfrom(g_listen_fd, buffer, BUF_SIZE - 1, 0, (struct sockaddr *)&cli_addr, &len);
        if (n < 0) {
            if (g_udp_stop_flag) break; // 正常停止
            log_message(1, "UDP: recvfrom() 错误: %s", strerror(errno));
            continue;
        }

        buffer[n] = '\0'; // 确保字符串结束
        handle_command(buffer);
    }

    close(g_listen_fd);
    g_listen_fd = -1;
    log_message(1, "UDP 服务器已停止。");
    return NULL;
}

int udp_server_start(int port) {
    int *port_arg = malloc(sizeof(int));
    if (!port_arg) {
        log_message(0, "UDP: 无法为端口参数分配内存。");
        return -1;
    }
    *port_arg = port;

    if (pthread_create(&g_udp_thread, NULL, udp_server_thread_func, port_arg) != 0) {
        log_message(0, "UDP: 无法创建服务器线程: %s", strerror(errno));
        free(port_arg);
        return -1;
    }
    return 0;
}

void udp_server_stop() {
    if (g_udp_thread) {
        g_udp_stop_flag = 1;
        // 通过关闭套接字来解除 recvfrom 的阻塞
        if (g_listen_fd != -1) {
            shutdown(g_listen_fd, SHUT_RDWR);
        }
        pthread_join(g_udp_thread, NULL);
        g_udp_thread = 0;
    }
}

int udp_check_and_clear_clean_request() {
    return atomic_exchange(&g_immediate_clean_request, 0);
}