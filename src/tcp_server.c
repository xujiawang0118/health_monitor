/*
 * epoll_chat_server.c — 多客户端聊天服务器（epoll 版，边缘触发）
 *
 * 流程: socket() → bind() → listen()
 *       epoll_create1 → 注册 sockfd(EPOLLIN)
 *       while(running) { epoll_wait → 遍历就绪事件 → accept/recv/send }
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tcp_server.h"



// /* ===== 客户端状态 ===== */
// static int clients[MAX_CLIENTS];
// static int client_count = 0;

// /* ===== 缓冲区 ===== */
// static char read_buf[BUF_SIZE];
// static char write_buf[BUF_SIZE];

void buf_init(char *read_buf,char *write_buf)
{
    memset(read_buf, 0, BUF_SIZE);
    memset(write_buf, 0, BUF_SIZE);
}

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    if (old_option < 0) {
        perror("fcntl(F_GETFL) 失败");
        return -1;
    }
    int new_option = old_option | O_NONBLOCK;
    if (fcntl(fd, F_SETFL, new_option) < 0) {
        perror("fcntl(F_SETFL) 失败");
        return -1;
    }
    return old_option;
}

/* 从 clients[] 数组中移除指定 fd（尾部替换法，O(1)） */
void remove_client(int fd, int *clients, int *client_count)
{
    for (int i = 0; i < *client_count; i++) {
        if (clients[i] == fd) {
            /* 用数组最后一个元素填补被删除的位置 */
            clients[i] = clients[*client_count - 1];
            (*client_count)--;
            return;
        }
    }
}

// int tcp_init()
// {

//     int sockfd, ret;

//     /* ---------- 1. 创建套接字 ---------- */
//     sockfd = socket(AF_INET, SOCK_STREAM, 0);
//     if (sockfd < 0) {
//         perror("socket() 失败");
//         exit(EXIT_FAILURE);
//     }

//     int opt = 1;
//     setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

//     struct sockaddr_in server_addr, client_addr;
//     socklen_t client_addr_len = sizeof(client_addr);
//     socklen_t server_addr_len = sizeof(server_addr);
//     memset(&server_addr, 0, server_addr_len);
//     memset(&client_addr, 0, client_addr_len);

//     /* 配置服务端地址 */
//     server_addr.sin_family      = AF_INET;
//     server_addr.sin_addr.s_addr = INADDR_ANY;
//     server_addr.sin_port        = htons(6666);

//     /* ---------- 2. 绑定 ---------- */
//     ret = bind(sockfd, (const struct sockaddr *)&server_addr, server_addr_len);
//     if (ret < 0) {
//         perror("bind() 失败");
//         close(sockfd);
//         exit(EXIT_FAILURE);
//     }

//     /* ---------- 3. 监听 ---------- */
//     ret = listen(sockfd, 128);
//     if (ret < 0) {
//         perror("listen() 失败");
//         close(sockfd);
//         exit(EXIT_FAILURE);
//     }

//     /* ---------- 4. 设为非阻塞 ---------- */
//     if (setnonblocking(sockfd) < 0) {
//         close(sockfd);
//         exit(EXIT_FAILURE);
//     }

//     /* ---------- 5. 创建 epoll 实例 ---------- */
//     int epollfd, nfds;
//     struct epoll_event ev, events[MAX_EVENTS];

//     epollfd = epoll_create1(0);
//     if (epollfd < 0) {
//         perror("epoll_create1() 失败");
//         close(sockfd);
//         exit(EXIT_FAILURE);
//     }

//     /* ---------- 6. 注册监听 fd ---------- */
//     ev.data.fd = sockfd;
//     ev.events  = EPOLLIN;
//     ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev);
//     if (ret < 0) {
//         perror("epoll_ctl(ADD sockfd) 失败");
//         close(epollfd);
//         close(sockfd);
//         exit(EXIT_FAILURE);
//     }

//     printf("╔════════════════════════════════════╗\n");
//     printf("║  多客户端聊天服务器 — epoll 版    ║\n");
//     printf("╠════════════════════════════════════╣\n");
//     printf("║ 监听端口: %-5d                    ║\n", 6666);
//     printf("║ 触发模式: 边缘触发 (EPOLLET)       ║\n");
//     printf("╚════════════════════════════════════╝\n\n");
//     printf("[系统] 等待客户端连接... (Ctrl+C 退出)\n\n");

    // /* ---------- 7. 事件循环 ---------- */
    // while (running) {
    //     nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    //     if (nfds < 0) {
    //         if (errno == EINTR) {
    //             continue;   /* 被信号打断，继续等待或退出 */
    //         }
    //         perror("epoll_wait() 失败");
    //         break;
    //     }

    //     for (int i = 0; i < nfds; i++) {
    //         int fd = events[i].data.fd;

    //         /* 检查异常事件 */
    //         if (events[i].events & (EPOLLERR | EPOLLHUP)) {
    //             printf("[系统] 客户端 fd=%d 异常 (EPOLLERR/EPOLLHUP)，断开连接\n", fd);
    //             epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    //             remove_client(fd);
    //             close(fd);
    //             continue;
    //         }

    //         if (fd == sockfd) {
    //             /* ---- 新连接 ---- */
    //             /*
    //              * 边缘触发 + 非阻塞 listen_fd：一次 epoll_wait 通知
    //              * 可能对应多个新连接，需要循环 accept 直到 EAGAIN。
    //              */
    //             while (1) {
    //                 client_addr_len = sizeof(client_addr);
    //                 int new_fd = accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
    //                 if (new_fd < 0) {
    //                     if (errno == EAGAIN || errno == EWOULDBLOCK) {
    //                         /* 当前没有更多新连接了 */
    //                         break;
    //                     }
    //                     perror("accept() 失败");
    //                     break;
    //                 }

    //                 if (client_count >= MAX_CLIENTS) {
    //                     printf("[系统] 客户端已满，拒绝 fd=%d 的连接\n", new_fd);
    //                     close(new_fd);
    //                     continue;
    //                 }

    //                 setnonblocking(new_fd);
    //                 clients[client_count++] = new_fd;

    //                 ev.data.fd = new_fd;
    //                 ev.events  = EPOLLIN | EPOLLET;
    //                 if (epoll_ctl(epollfd, EPOLL_CTL_ADD, new_fd, &ev) < 0) {
    //                     perror("epoll_ctl(ADD client) 失败");
    //                     remove_client(new_fd);
    //                     close(new_fd);
    //                     continue;
    //                 }

    //                 printf("[连接] 客户端 %s:%d (fd=%d) 已连接，当前在线: %d\n",
    //                        inet_ntoa(client_addr.sin_addr),
    //                        ntohs(client_addr.sin_port),
    //                        new_fd, client_count);
    //             }
    //         } else if (events[i].events & EPOLLIN) {
    //             /* ---- 客户端数据 ---- */
    //             int count, send_count;

    //             /*
    //              * 边缘触发：必须循环 recv 直到返回 EAGAIN，
    //              * 否则剩余数据要等下次 epoll_wait 通知（可能永远不来）。
    //              */
    //             while (1) {
    //                 buf_init();
    //                 count = recv(fd, read_buf, BUF_SIZE - 1, 0);

    //                 if (count > 0) {
    //                     printf("[来自 fd=%d] %s", fd, read_buf);

    //                     strcpy(write_buf, "ack\n");
    //                     send_count = send(fd, write_buf, strlen(write_buf), 0);
    //                     if (send_count < 0) {
    //                         if (errno == EAGAIN || errno == EWOULDBLOCK) {
    //                             /* 发送缓冲区满，丢弃本次回复 */
    //                             continue;
    //                         }
    //                         perror("send() 失败");
    //                         break;
    //                     }
    //                 } else if (count == 0) {
    //                     /* 对端关闭连接（收到 FIN） */
    //                     printf("[断开] 客户端 fd=%d 请求关闭连接\n", fd);
    //                     epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    //                     remove_client(fd);
    //                     close(fd);
    //                     printf("[系统] 已移除 fd=%d，当前在线: %d\n", fd, client_count);
    //                     break;  /* 退出 while(1) recv 循环 */
    //                 } else {
    //                     /* count == -1 */
    //                     if (errno == EAGAIN || errno == EWOULDBLOCK) {
    //                         /*
    //                          * 边缘触发：数据已读完，正常退出 recv 循环。
    //                          * 不是错误。
    //                          */
    //                         break;
    //                     }
    //                     if (errno == EINTR) {
    //                         continue;   /* 被信号打断，重试 */
    //                     }
    //                     perror("recv() 失败");
    //                     /* 连接可能已断开，做清理 */
    //                     epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    //                     remove_client(fd);
    //                     close(fd);
    //                     printf("[系统] fd=%d recv 错误，已断开，当前在线: %d\n", fd, client_count);
    //                     break;
    //                 }
    //             }
    //         }
    //     }
    // }

//     /* ---------- 8. 清理 ---------- */
//     printf("\n[系统] 服务端正在退出...\n");
//     for (int i = 0; i < client_count; i++) {
//         close(clients[i]);
//     }
//     close(epollfd);
//     close(sockfd);
//     printf("[系统] 资源已释放，服务端退出\n");

//     return 0;
// }
