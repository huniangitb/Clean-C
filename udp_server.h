// udp_server.h
#ifndef UDP_SERVER_H
#define UDP_SERVER_H

// 启动 UDP 服务器线程
// port: 监听的端口号
// 返回: 0 表示成功, -1 表示失败
int udp_server_start(int port);

// 停止 UDP 服务器线程
void udp_server_stop();

// 检查是否有来自 UDP 的立即清理请求
// 返回: 1 表示有请求, 0 表示无请求
int udp_check_and_clear_clean_request();

#endif // UDP_SERVER_H