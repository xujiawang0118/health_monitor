/**
 * @file    main.c
 * @brief   健康监测终端 — 主入口
 *
 * 模块化后的 main.c 只负责：
 *   1. 传感器线程（MAX30102 中断驱动 + MPU6050 轮询）
 *   2. TCP 服务端线程
 *   3. 线程创建/等待/资源清理
 *
 * MQTT、HTTP、显示等模块独立为单独文件，由 main 统一编排。
 */

#include "globals.h"    /* 必须在系统头文件之前——引入特性宏 */

#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "i2c_utils.h"
#include "mpu6050.h"
#include "max30102.h"
#include "tcp_server.h"
#include "mqtt_client.h"
#include "http_server.h"
#include "display.h"

/*
 * TCP epoll_wait 超时时间（毫秒）。
 *
 * 多线程下 SIGINT 由内核投递给任意一个线程，TCP 线程不一定收到。
 * 如果 TCP 线程没收到信号，epoll_wait(timeout=-1) 就永远不返回，
 * 主线程卡在 pthread_join 上，进程无法退出。
 *
 * 加 500ms 超时后，即使没收到信号，TCP 线程也会定期醒来检查
 * g_running 标志位，从而正常退出。
 */
#define TCP_EPOLL_TIMEOUT_MS  500


/* ═══════════════════════════════════════════════════════════════════
 *  MAX30102 传感器线程（GPIO 中断驱动）
 *
 *  poll 监听 MAX30102 中断引脚，中断触发后读取 FIFO 并处理 PPG 信号。
 *  计算结果写入全局变量 g_latest_ppg，供 MQTT/HTTP/Display 线程读取。
 * ═══════════════════════════════════════════════════════════════════ */
static void *thread_max30102(void *arg)
{
    int max30102_fd = *(int *)arg;
    while (g_running) {
        struct pollfd pfd = { .fd = max30102_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) continue;   /* 被信号打断，继续 */
            perror("poll");
            return NULL;
        }
        if (ret > 0) {
            max30102_int_read_event();
        }

        int fifo_count = max30102_get_fifo_count(g_i2c_fd, MAX30102_DEV_ADDR);
        if (fifo_count > 0) {
            for (int i = 0; i < fifo_count; i++) {
                PPG_RawData_t raw;
                if (max30102_read_fifo(g_i2c_fd, MAX30102_DEV_ADDR, &raw) == 0) {
                    PPG_Result_t res = ppg_process(raw, get_sys_ms());

                    pthread_mutex_lock(&g_ppg_lock);
                    if (res.data_valid) {
                        g_latest_ppg = res;
                    }
                    g_ppg_fill = ppg_get_fill_count();
                    pthread_mutex_unlock(&g_ppg_lock);
                }
            }
        }
    }
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════
 *  MPU6050 传感器线程（定时轮询）
 *
 *  每 MPU6050_INTERVAL_MS 毫秒读取一次原始数据并转换为物理量。
 *  结果写入全局变量 g_latest_mpu / g_latest_mpu_raw。
 * ═══════════════════════════════════════════════════════════════════ */
static void *thread_mpu6050(void *arg)
{
    int mpu6050_fd = *(int *)arg;
    while (g_running) {
        usleep(MPU6050_INTERVAL_MS * 1000);

        mpu6050_raw_data_t raw;
        mpu6050_data_t data;
        if (mpu6050_read_raw(mpu6050_fd, MPU6050_DEFAULT_ADDR, &raw) == 0) {
            mpu6050_convert(&raw, &data, MPU6050_ACCEL_SENS_2G, MPU6050_GYRO_SENS_250);

            pthread_mutex_lock(&g_mpu_lock);
            g_latest_mpu_raw = raw;
            g_latest_mpu = data;
            pthread_mutex_unlock(&g_mpu_lock);
        }
    }
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════
 *  TCP 服务端线程（epoll 边缘触发）
 *
 *  监听 6666 端口，接受多个客户端连接，每 500ms 广播一次传感器 JSON。
 *  客户端断开自动清理。
 * ═══════════════════════════════════════════════════════════════════ */
static void *thread_tcp_server(void *arg)
{
    (void)arg;

    int sockfd, ret;

    /* 1. 创建套接字 */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket() 失败");
        return NULL;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    socklen_t server_addr_len = sizeof(server_addr);
    memset(&server_addr, 0, server_addr_len);
    memset(&client_addr, 0, client_addr_len);

    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(6666);

    /* 2. 绑定 */
    ret = bind(sockfd, (const struct sockaddr *)&server_addr, server_addr_len);
    if (ret < 0) {
        perror("bind() 失败");
        close(sockfd);
        return NULL;
    }

    /* 3. 监听 */
    ret = listen(sockfd, 128);
    if (ret < 0) {
        perror("listen() 失败");
        close(sockfd);
        return NULL;
    }

    /* 4. 设为非阻塞 */
    if (setnonblocking(sockfd) < 0) {
        close(sockfd);
        return NULL;
    }

    /* 5. 创建 epoll 实例 */
    int epollfd, nfds;
    struct epoll_event ev, events[MAX_EVENTS];

    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        perror("epoll_create1() 失败");
        close(sockfd);
        return NULL;
    }

    /* 6. 注册监听 fd（边缘触发） */
    ev.data.fd = sockfd;
    ev.events  = EPOLLIN;
    ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev);
    if (ret < 0) {
        perror("epoll_ctl(ADD sockfd) 失败");
        close(epollfd);
        close(sockfd);
        return NULL;
    }

    tcp_log("╔════════════════════════════════════╗");
    tcp_log("║  TCP 服务端已启动                  ║");
    tcp_log("╠════════════════════════════════════╣");
    tcp_log("║ 监听端口: %-5d                    ║", 6666);
    tcp_log("║ 触发模式: 边缘触发 (EPOLLET)       ║");
    tcp_log("╚════════════════════════════════════╝");
    tcp_log("等待客户端连接...");

    /* 7. 事件循环 */
    while (g_running) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, TCP_EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait() 失败");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                tcp_log("[系统] 客户端 fd=%d 异常，断开连接", fd);
                epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
                remove_client(fd, clients, &client_count);
                close(fd);
                continue;
            }

            if (fd == sockfd) {
                /* 新连接（边缘触发需循环 accept 直到 EAGAIN） */
                while (1) {
                    client_addr_len = sizeof(client_addr);
                    int new_fd = accept(sockfd,
                                        (struct sockaddr *)&client_addr,
                                        &client_addr_len);
                    if (new_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept() 失败");
                        break;
                    }

                    if (client_count >= MAX_CLIENTS) {
                        tcp_log("[系统] 客户端已满，拒绝 fd=%d", new_fd);
                        close(new_fd);
                        continue;
                    }

                    setnonblocking(new_fd);
                    clients[client_count++] = new_fd;

                    ev.data.fd = new_fd;
                    ev.events  = EPOLLIN | EPOLLET;
                    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, new_fd, &ev) < 0) {
                        perror("epoll_ctl(ADD client) 失败");
                        remove_client(new_fd, clients, &client_count);
                        close(new_fd);
                        continue;
                    }

                    tcp_log("[连接] 客户端 %s:%d (fd=%d)，在线: %d",
                            inet_ntoa(client_addr.sin_addr),
                            ntohs(client_addr.sin_port),
                            new_fd, client_count);
                }
            } else if (events[i].events & EPOLLIN) {
                /* 客户端数据（边缘触发需循环 recv 直到 EAGAIN） */
                while (1) {
                    buf_init(read_buf, write_buf);
                    int count = recv(fd, read_buf, BUF_SIZE - 1, 0);

                    if (count > 0) {
                        tcp_log("[来自 fd=%d] %s", fd, read_buf);
                    } else if (count == 0) {
                        tcp_log("[断开] 客户端 fd=%d 请求关闭连接", fd);
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
                        remove_client(fd, clients, &client_count);
                        close(fd);
                        tcp_log("[系统] 已移除 fd=%d，在线: %d", fd, client_count);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("recv() 失败");
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
                        remove_client(fd, clients, &client_count);
                        close(fd);
                        tcp_log("[系统] fd=%d recv 错误，已断开，在线: %d",
                                fd, client_count);
                        break;
                    }
                }
            }
        }

        /* 向所有在线客户端广播传感器 JSON */
        if (client_count > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t timestamp_ms = (uint64_t)ts.tv_sec * 1000
                                   + ts.tv_nsec / 1000000;

            pthread_mutex_lock(&g_mpu_lock);
            mpu6050_data_t mpu = g_latest_mpu;
            pthread_mutex_unlock(&g_mpu_lock);

            pthread_mutex_lock(&g_ppg_lock);
            PPG_Result_t ppg = g_latest_ppg;
            pthread_mutex_unlock(&g_ppg_lock);

            char json[512];
            snprintf(json, sizeof(json),
                "{\"t\":%llu,\"mpu\":{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp\":%.1f},"
                "\"ppg\":{\"hr\":%d,\"spo2\":%d,\"valid\":%d}}\n",
                (unsigned long long)timestamp_ms,
                mpu.accel_x_g, mpu.accel_y_g, mpu.accel_z_g,
                mpu.gyro_x_dps, mpu.gyro_y_dps, mpu.gyro_z_dps, mpu.temp_c,
                ppg.heart_rate, ppg.spo2, ppg.data_valid
            );

            size_t json_len = strlen(json);
            for (int c = 0; c < client_count; c++) {
                ssize_t sent = send(clients[c], json, json_len, 0);
                if (sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    tcp_log("[错误] send fd=%d 失败，断开连接", clients[c]);
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, clients[c], NULL);
                    close(clients[c]);
                    remove_client(clients[c], clients, &client_count);
                    c--;  /* 删除后索引回退 */
                }
            }
        }
    }

    /* 清理 */
    for (int i = 0; i < client_count; i++)
        close(clients[i]);
    close(epollfd);
    close(sockfd);

    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════
 *  main — 总入口
 *
 *  初始化顺序：I2C → 传感器 → 创建 6 个线程 → join → 清理
 * ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char const *argv[])
{
    (void)argc;
    (void)argv;

    int ret;

    /* 注册信号处理 */
    if (setup_signals() < 0)
        return EXIT_FAILURE;

    /* 打开 I2C 总线 */
    g_i2c_fd = i2c_open(I2C_BUS_NUM);
    if (g_i2c_fd < 0) {
        fprintf(stderr, "[ERROR] 无法打开 /dev/i2c-%d\n", I2C_BUS_NUM);
        return EXIT_FAILURE;
    }

    /* 初始化 MPU6050 */
    ret = mpu6050_init(g_i2c_fd, MPU6050_DEFAULT_ADDR);
    if (ret < 0) {
        fprintf(stderr, "[ERROR] MPU6050 初始化失败\n");
        i2c_close(g_i2c_fd);
        return EXIT_FAILURE;
    }

    /* 初始化 MAX30102 */
    ret = max30102_init(g_i2c_fd, MAX30102_DEV_ADDR);
    if (ret < 0) {
        fprintf(stderr, "[ERROR] MAX30102 初始化失败。\n"
                        "  请检查接线（VCC/GND/SCL/SDA/INT），"
                        "以及在板端执行 'sudo i2cdetect -y %d'\n",
                        I2C_BUS_NUM);
        i2c_close(g_i2c_fd);
        return EXIT_FAILURE;
    }

    /* 初始化 MAX30102 GPIO 中断引脚 */
    int max30102_fd = max30102_int_init();
    if (max30102_fd < 0) {
        fprintf(stderr, "[ERROR] MAX30102 中断引脚初始化失败\n");
        i2c_close(g_i2c_fd);
        return EXIT_FAILURE;
    }

    hide_cursor();

    /* 创建 6 个工作线程 */
    pthread_t max30102_thread, mpu6050_thread, display_thread;
    pthread_t tcp_thread, http_thread, mqtt_thread;

    pthread_create(&max30102_thread, NULL, thread_max30102, &max30102_fd);
    pthread_create(&mpu6050_thread, NULL, thread_mpu6050,   &g_i2c_fd);
    pthread_create(&display_thread,   NULL, thread_display,   NULL);
    pthread_create(&tcp_thread,       NULL, thread_tcp_server, NULL);
    pthread_create(&http_thread,      NULL, thread_http_server, NULL);
    pthread_create(&mqtt_thread,      NULL, thread_mqtt,      NULL);

    /* 等待所有线程退出 */
    pthread_join(max30102_thread, NULL);
    pthread_join(mpu6050_thread, NULL);
    pthread_join(display_thread,   NULL);
    pthread_join(tcp_thread,       NULL);
    pthread_join(http_thread,      NULL);
    pthread_join(mqtt_thread,      NULL);

    /* 清理资源 */
    max30102_int_cleanup();
    show_cursor();
    i2c_close(g_i2c_fd);

    return EXIT_SUCCESS;
}
