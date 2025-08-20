// udp_client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9038 // 与后端监听的端口保持一致

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "用法: %s <命令>\n", argv[0]);
        fprintf(stderr, "支持的命令: clean_now, start_gc, stop_gc\n");
        return 1;
    }

    const char *command = argv[1];

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);
    if (inet_aton(SERVER_IP, &servaddr.sin_addr) == 0) {
        fprintf(stderr, "inet_aton() failed\n");
        close(sockfd);
        return 1;
    }

    if (sendto(sockfd, command, strlen(command), 0, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("sendto failed");
        close(sockfd);
        return 1;
    }

    // UDP 是无连接的，我们不需要等待响应，直接退出
    // printf("命令 '%s' 已发送。\n", command);

    close(sockfd);
    return 0;
}