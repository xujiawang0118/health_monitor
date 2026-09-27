/**
 * @file    main.c
 * @brief   健康监测终端 — 主入口
 *
 * 模块化后的 main.c 只负责：
 *   1. 传感器与中断初始化
 *   2. 看门狗接管（必须在创建工作线程之前）
 *   3. 7 个线程的创建与带超时回收
 *   4. 退出时的资源清理与看门狗归还
 *
 * 各功能线程（MAX30102 / MPU6050 / 显示 / TCP / HTTP / MQTT / 看门狗巡检）
 * 的实现分散在对应模块文件中，由本文件统一编排。
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
#include <time.h>
#include "i2c_utils.h"
#include "mpu6050.h"
#include "max30102.h"
#include "tcp_server.h"
#include "mqtt_client.h"
#include "http_server.h"
#include "display.h"
#include "watchdog.h"

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
        wd_beat(WD_CH_MAX30102);   /* 心跳：每轮循环开头打一次，见 watchdog.c */

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


/* ─── 把 errno 翻译成 I2C 路径上真会见到的短标签 ─── */
/*
 * 不用 strerror()：glibc 的 strerror() 返回的是**静态缓冲区**的指针，
 * 不是线程安全的。i2c_utils.c 的失败分支里也在调 strerror()（另一个线程），
 * 两边会互相覆盖，日志里可能打出张冠李戴的错误名 —— 排查故障时这比不打印
 * 还糟。这里用查表，纯常量字符串，无共享状态。
 *
 * 标签写成"中文说明 + 英文名"，是为了让日志本身自带注解 —— 事后翻日志
 * 不用再去查 errno 表。各值的来历（在 i2c-imx.c 里都能找到出处）：
 *   EAGAIN    I2SR_IAL 仲裁丢失位：总线上有器件把 SDA 拽住不放
 *   ETIMEDOUT i2c_imx_bus_busy() 里 500ms 超时：总线被占住太久
 *   ENXIO     从机地址无应答（NACK）
 *   EIO       其它传输错误
 *
 * 注意标签总长度受环形缓冲（TCP_LOG_MAXLEN=128 字节）和终端宽度双重
 * 约束，别再往里加字。
 */
static const char *i2c_errno_tag(int e)
{
    switch (e) {
    case 0:         return "正常";
    case EAGAIN:    return "仲裁丢失EAGAIN";
    case ETIMEDOUT: return "总线超时ETIMEDOUT";
    case ENXIO:     return "从机无应答ENXIO";
    case EIO:       return "传输错误EIO";
    default:        return "其它错误";
    }
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
    int fail_count = 0;      /* 连续读异常的计数，达到阈值就软复位器件 */
    int last_kind  = 0;      /* 上一次记录的异常类型，用于捕捉"类型转折" */

    while (g_running) {
        /*
         * 心跳打在循环**开头**而不是结尾：这样"我活着、马上要干活了"
         * 和"我刚干完活"是同一个含义 —— 心跳的时间间隔就等于一轮循环
         * 的真实耗时。总线被拉死时一轮能拖到十几秒，阈值就是照着这个
         * 上限配的（见 config.h 里 WATCHDOG_STALE_TICKS 的说明）。
         */
        wd_beat(WD_CH_MPU6050);

        usleep(MPU6050_INTERVAL_MS * 1000);

        mpu6050_raw_data_t raw;
        mpu6050_data_t data;

        /*
         * 两种 I2C 失效必须在代码里分开判定，否则日志读不出故障是怎么演变的：
         *
         *   读全0   —— mpu6050_read_raw() 返回 0（ioctl 成功返回 2），只是读回
         *              14 个 0x00。坏的是从机内部寄存器/时钟状态，I2C 电气层
         *              完好。影响范围只有 MPU6050 自己，软复位（H_RESET）能救。
         *
         *   ioctl失败 —— i2c-imx 返回 -1、errno=EAGAIN(11)，唯一来源是
         *              i2c-imx.c 的 I2SR_IAL 仲裁丢失位：总线上有器件把 SDA
         *              拽住不放。影响范围是整条 i2c-0（含 GT1151 触摸屏），
         *              软复位救不了 —— 复位自己也要走这条已经死掉的总线。
         *
         * 这里绝不能用 ret = -1 把两者压成同一个值：压掉之后就再也分不清
         * "先持续全 0 再翻成 ioctl 失败"（同一起故障的两个阶段）和
         * "从头到尾就是 ioctl 失败"（纯粹是总线被拽死）。
         */
        /*
         * 读之前先把 errno 清零 —— 这一步不是可有可无的。
         *
         * i2c_read_reg() 内部有 5 次重试：中途失败、后续某次重试成功时，
         * 它直接 return 0，**不会清理 errno**。失败那次的错误码就永远留在
         * 本线程的 errno 里（errno 是"粘"的，一旦置上不会自己清零）。
         *
         * 不清零的话，"读全0"这类日志后面跟的 errno 就是上一次故障的陈年
         * 残值，什么时候发生的完全看不出来 —— 纯噪声，还容易误导人以为
         * "这次读超时了"。
         *
         * 清零之后，这一行的 errno 才有确定含义：
         *   errno == 0    → ioctl 一次就成功，数据是器件真实回的全 0
         *   errno != 0    → 这次读在重试后才成功，中途总线出过错
         * 这两者指向的原因完全不同，正是要区分的东西。
         */
        errno = 0;
        int ret = mpu6050_read_raw(mpu6050_fd, MPU6050_DEFAULT_ADDR, &raw);
        int saved_errno = errno;   /* 紧接着取，防止后续调用改写 errno */

        int kind = 0;              /* 0=正常, 1=读全0, 2=ioctl失败 */
        const char *kind_str = NULL;

        if (ret < 0) {
            kind = 2;
            kind_str = "ioctl失败";
        } else if (mpu6050_raw_all_zero(&raw)) {
            kind = 1;
            kind_str = "读全0";
        }

        if (kind == 0) {
            fail_count = 0;
            last_kind  = 0;
            mpu6050_convert(&raw, &data, MPU6050_ACCEL_SENS_2G, MPU6050_GYRO_SENS_250);

            pthread_mutex_lock(&g_mpu_lock);
            g_latest_mpu_raw = raw;
            g_latest_mpu = data;
            pthread_mutex_unlock(&g_mpu_lock);
            continue;
        }

        fail_count++;

        /*
         * 只在"异常开始"和"异常类型发生转折"时记录，不是每次失败都记。
         *
         * 为什么不全记：本线程 50ms 一跳，异常时就是每秒 20 行；而 tcp_log
         * 环形缓冲只有 TCP_LOG_LINES(6) 行，显示线程每 500ms 整块重绘。全记
         * 会把缓冲冲爆，屏幕上只剩最近 0.3 秒的内容，同时把 TCP/HTTP/MQTT 的
         * 日志全挤出去 —— 恰恰丢掉了我们要重建的那条时间线。
         *
         * 记"转折点"的信息量和全记等价：模式①→模式② 的那一刻一定会打出一条
         * 类型变化的日志，而这正是判定两个故障是否同源的关键证据。
         */
        /*
         * 把 saved_errno 翻译成一句人能读的话。
         *
         * "读全0"要分两小类：ioctl 一次成功 / 重试后才成功。这两者指向的
         * 原因完全不同（见上面 errno 清零那段注释），是判断故障性质的关键。
         */
        char why[64];
        if (kind == 1) {
            if (saved_errno == 0)
                snprintf(why, sizeof(why), "ioctl一次成功");
            else
                snprintf(why, sizeof(why), "ioctl重试后才成功(上次: %s)",
                         i2c_errno_tag(saved_errno));
        } else {
            snprintf(why, sizeof(why), "errno=%d %s",
                     saved_errno, i2c_errno_tag(saved_errno));
        }

        if (last_kind == 0) {
            tcp_log("[MPU6050] 异常开始(%s) %s", kind_str, why);
        } else if (last_kind != kind) {
            /*
             * 转折行故意**不带** errno 的中文注解，只留数字。
             *
             * 两个原因：
             *   1. 它紧跟在"异常开始"行后面，两行同时在环形缓冲里，
             *      注解不会丢，重复一遍是浪费；
             *   2. 带上注解这行会顶到 80 列以上。显示线程是整屏重绘
             *      （\033[H\033[J）然后逐行 \033[K 清到行尾的，行一换行
             *      清行就落到了下一行，整个屏幕开始错位。
             */
            tcp_log("[MPU6050] 转折 %s->%s errno=%d 共%d次",
                    (last_kind == 1) ? "读全0" : "ioctl失败",
                    kind_str, saved_errno, fail_count);
        }
        last_kind = kind;

        if (fail_count >= MPU6050_FAIL_RESET_THRESHOLD) {
            /*
             * 连续失败达到阈值 → 软复位器件，把它从"全0/坏状态"拉回来。
             * 复位也要走 I2C，总线真被拽死时这里会同样失败（日志上表现为
             * 紧跟着一条 i2c_write_reg 的 errno=11）；但主线程带超时的 join
             * 能保证进程仍可退出，不会像之前那样卡死。
             */
            tcp_log("[MPU6050] 连续 %d 次异常(%s)，触发软复位",
                    fail_count, kind_str);
            mpu6050_reset(mpu6050_fd, MPU6050_DEFAULT_ADDR);
            fail_count = 0;
        }
    }
    return NULL;
}

#ifdef SENSOR_MOCK
/* mock：周期性生成假 MPU6050 物理量（步进值模拟姿态变化） */
static void *thread_mpu6050_mock(void *arg)
{
    (void)arg;
    while (g_running) {
        wd_beat(WD_CH_MPU6050);
        usleep(MPU6050_INTERVAL_MS * 1000);

        mpu6050_raw_data_t raw;
        mpu6050_data_t data;

        /* 模拟 MPU6050 数据 */
        raw.accel_x = rand() % 32768 - 16384;
        raw.accel_y = rand() % 32768 - 16384;
        raw.accel_z = rand() % 32768 - 16384;
        raw.gyro_x  = rand() % 32768 - 16384;
        raw.gyro_y  = rand() % 32768 - 16384;
        raw.gyro_z  = rand() % 32768 - 16384;
        raw.temp = rand() % 340 - 170; 

        mpu6050_convert(&raw, &data, MPU6050_ACCEL_SENS_2G, MPU6050_GYRO_SENS_250);

        pthread_mutex_lock(&g_mpu_lock);
        g_latest_mpu_raw = raw;
        g_latest_mpu = data;
        pthread_mutex_unlock(&g_mpu_lock);
    }
    return NULL;
}

/* mock：周期性生成假 PPG 结果（心率围绕 72 小范围波动） */
static void *thread_max30102_mock(void *arg)
{
    (void)arg;
    while (g_running) {
        wd_beat(WD_CH_MAX30102);
        usleep(50000);                               /* 20Hz，模拟 FIFO 节奏 */
        PPG_Result_t res;
        res.heart_rate = 72 + (rand() % 5);          /* 72~76 bpm */
        res.spo2 = 98;
        res.data_valid = true;

        pthread_mutex_lock(&g_ppg_lock);
        g_latest_ppg = res;
        g_ppg_fill = PPG_WINDOW_SIZE;
        pthread_mutex_unlock(&g_ppg_lock);
    }
    return NULL;
}
#endif /* SENSOR_MOCK */


/* ═══════════════════════════════════════════════════════════════════
 *  TCP 服务端线程（epoll 边缘触发）
 *
 *  监听 6666 端口，接受多个客户端连接，每 500ms 广播一次传感器 JSON。
 *  客户端断开自动清理。
 * ═══════════════════════════════════════════════════════════════════ */
static void *thread_tcp_server(void *arg)
{
    (void)arg;

    uint64_t last_publish = 0;


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
        wd_beat(WD_CH_TCP);

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
        uint64_t now = get_sys_ms();
        if (client_count > 0 && now - last_publish >= 500) {
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
            
            //含半包处理
            for (int c = 0; c < client_count; c++) {
                size_t total_sent = 0;
                while(total_sent < json_len)
                {
                    ssize_t sent = send(clients[c], json + total_sent, json_len-total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;   /* 缓冲满，放弃这帧，下帧再来（不能 continue 忙等） */

                        /* 真实错误：断开该客户端 */
                        tcp_log("[错误] send fd=%d 失败，断开连接", clients[c]);
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, clients[c], NULL);
                        close(clients[c]);
                        remove_client(clients[c], clients, &client_count);
                        c--;   /* 删除后索引回退 */
                        break;
                    }
                    total_sent += sent;
                }
            
            }
            last_publish = now;
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
 *  带超时的线程回收
 *
 *  **所有**线程都必须用这个函数回收，不能用裸的 pthread_join。
 *
 *  本程序的退出是协作式的：每个线程都要自己回到循环顶部检查 g_running。
 *  只要有一个线程回不去，裸 pthread_join 就会把主线程一起拖住，整个进程
 *  永远退不出去 —— 用户只能拔电。已知会回不去的路径：
 *
 *    - 传感器线程：总线被从机拉死时，i2c-imx 控制器会长时间忙等，
 *      阻塞的 ioctl 迟迟不返回；
 *    - MQTT 线程：mosquitto_connect() 是阻塞调用（DNS + TCP + TLS 握手 +
 *      等 CONNACK），整个过程中不检查我们的标志位，断网时能卡几十秒；
 *    - HTTP 线程：MHD_stop_daemon() 没有超时参数，浏览器连接没断干净时
 *      可能阻塞。
 *
 *  这里用 pthread_timedjoin_np（GNU 扩展，板端 glibc 2.28 支持）：
 *  最多等 timeout_sec 秒，超时就放弃等待、打印告警后返回 -1。
 *
 *  返回值：0 = 线程已回收（正常退出或早已自行结束）
 *          -1 = 超时，线程还活着，调用方不能去动它持有的资源
 * ═══════════════════════════════════════════════════════════════════ */
static int join_with_timeout(pthread_t tid, const char *name, long timeout_sec)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        pthread_join(tid, NULL);   /* 拿不到时间就退化为普通 join */
        return 0;
    }
    ts.tv_sec += timeout_sec;

    int ret = pthread_timedjoin_np(tid, NULL, &ts);
    if (ret == ETIMEDOUT) {
        fprintf(stderr,
                "[WARN] 线程 %s 在 %ld 秒内未退出，跳过等待\n",
                name, timeout_sec);
        return -1;
    }
    /* ret==0 正常回收；ESRCH 线程已自行退出 */
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 *  main — 总入口
 *
 *  初始化顺序：I2C → 传感器 → wd_init → 创建 7 个线程 → join → 清理
 * ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char const *argv[])
{
    (void)argc;
    (void)argv;

    int ret;

    /* 注册信号处理 */
        if (setup_signals() < 0)
            return EXIT_FAILURE;
        
    #ifdef SENSOR_MOCK
    g_i2c_fd = -1;   /* mock 下不打开真实 I2C，句柄置为非法值 */
    tcp_log("[MOCK] 传感器 mock 模式，跳过硬件初始化");
    srand((unsigned)time(NULL)); // 增加随机种子

    #else
    

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
    #endif

    hide_cursor();

    /*
     * 打开硬件看门狗。
     *
     * 位置是刻意选的：传感器初始化之后、创建线程之前。
     *   - 放传感器初始化之后：那一段要碰 I2C，慢且可能失败；失败时进程
     *     直接 return，不该已经背上"卡住就复位整板"的负担。
     *   - 放创建线程之前：所有会卡死的工作线程都必须在看门狗已经接管之后
     *     才能跑起来，否则中间那段时间是裸奔的。
     *
     * 注意 open(/dev/watchdog) 本身就把硬件计数器启动了（超时先按驱动默认
     * 的 60 秒，wd_init 内部紧接着改成 WATCHDOG_TIMEOUT_S）。所以从这一行
     * 之后，代码必须真的能走到 wd_stop()，或者由看门狗兜底。
     */
    wd_init();

    /* 创建 7 个工作线程（6 个功能线程 + 1 个看门狗巡检线程） */
    pthread_t max30102_thread, mpu6050_thread, display_thread;
    pthread_t tcp_thread, http_thread, mqtt_thread, wd_thread;

    #ifdef SENSOR_MOCK
    int rc_max30102 = pthread_create(&max30102_thread, NULL, thread_max30102_mock, NULL);
    int rc_mpu6050  = pthread_create(&mpu6050_thread,  NULL, thread_mpu6050_mock,  NULL);

    #else
    int rc_max30102 = pthread_create(&max30102_thread, NULL, thread_max30102, &max30102_fd);
    int rc_mpu6050  = pthread_create(&mpu6050_thread,  NULL, thread_mpu6050,  &g_i2c_fd);
    #endif
    int rc_display  = pthread_create(&display_thread,   NULL, thread_display,  NULL);
    int rc_tcp      = pthread_create(&tcp_thread,       NULL, thread_tcp_server, NULL);
    int rc_http     = pthread_create(&http_thread,      NULL, thread_http_server, NULL);
    int rc_mqtt     = pthread_create(&mqtt_thread,      NULL, thread_mqtt,     NULL);
    int rc_wd       = pthread_create(&wd_thread,        NULL, thread_watchdog, NULL);

    /* 任一创建失败：打印错误并让已创建的线程退出（它们看到 g_running=0 后自行结束）。
     * 用 strerror(rc) 而非 perror()：pthread_create 失败时不设置 errno，
     * 错误码就是返回值本身，perror 会打印陈旧的 errno 造成误导。 */
    if (rc_max30102) { fprintf(stderr, "[ERROR] pthread_create(max30102): %s\n", strerror(rc_max30102)); g_running = 0; }
    if (rc_mpu6050)  { fprintf(stderr, "[ERROR] pthread_create(mpu6050): %s\n",  strerror(rc_mpu6050));  g_running = 0; }
    if (rc_display)  { fprintf(stderr, "[ERROR] pthread_create(display): %s\n",  strerror(rc_display));  g_running = 0; }
    if (rc_tcp)      { fprintf(stderr, "[ERROR] pthread_create(tcp): %s\n",      strerror(rc_tcp));      g_running = 0; }
    if (rc_http)     { fprintf(stderr, "[ERROR] pthread_create(http): %s\n",     strerror(rc_http));     g_running = 0; }
    if (rc_mqtt)     { fprintf(stderr, "[ERROR] pthread_create(mqtt): %s\n",     strerror(rc_mqtt));     g_running = 0; }
    /*
     * 巡检线程创建失败是这里唯一"后果会外溢到板子"的失败：看门狗已经
     * 打开在跑，却没人喂，30 秒后整板复位。所以必须置 g_running = 0 走正常
     * 收尾，让 wd_stop() 把设备交还给内核补喂。
     */
    if (rc_wd)       { fprintf(stderr, "[ERROR] pthread_create(watchdog): %s\n", strerror(rc_wd));     g_running = 0; }


    /*
     * 主线程在这里等退出信号 —— 这一步以前是裸 pthread_join 隐式提供的：
     * 主线程阻塞着，直到 Ctrl+C 把 g_running 置 0、各线程退出，join 才返回。
     *
     * 改成带超时的 join 之后，这层"阻塞等待"就丢了：线程还在正常跑，
     * 2 秒超时却照样到期，主线程误判"有线程退不出去"，于是程序启动
     * 12 秒后（6 × 2 秒）必然自己 _exit() 掉。
     *
     * 带超时的 join 只负责**退出阶段**兜底，不能拿来当主循环。
     */
    while (g_running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }




    /*
     * 只 join 创建成功的线程（失败的那个 pthread_t 未定义，不能 join）。
     *
     * 六个线程全部走带超时的 join。不要觉得"网络/显示线程不碰 I2C，
     * 普通 join 就行" —— MQTT 线程会卡在阻塞的 mosquitto_connect()，
     * HTTP 线程会卡在没有超时的 MHD_stop_daemon()，一样能让进程退不出去。
     *
     * 任何一个超时都记到 all_joined = 0，下面据此跳过资源清理。
     */
    int all_joined = 1;

    /*
     * 巡检线程排在最前面收：它在 g_running 置 0 后最多 2 秒（一个巡检
     * 周期）就自己退出，先把它 join 掉，省得它占着 2 秒超时额度。
     * 它退出后这段时间没人喂狗 —— 但收尾全程有界（下面 6 次 join 各
     * 最多 2 秒 + 几个不阻塞的清理），远小于 WATCHDOG_TIMEOUT_S，安全。
     */
    if (rc_wd       == 0 && join_with_timeout(wd_thread,       "watchdog",3) != 0)
        all_joined = 0;
    if (rc_max30102 == 0 && join_with_timeout(max30102_thread, "max30102", 2) != 0)
        all_joined = 0;
    if (rc_mpu6050  == 0 && join_with_timeout(mpu6050_thread,  "mpu6050",  2) != 0)
        all_joined = 0;
    if (rc_display  == 0 && join_with_timeout(display_thread,  "display",  2) != 0)
        all_joined = 0;
    if (rc_tcp      == 0 && join_with_timeout(tcp_thread,      "tcp",      2) != 0)
        all_joined = 0;
    if (rc_http     == 0 && join_with_timeout(http_thread,     "http",     2) != 0)
        all_joined = 0;
    if (rc_mqtt     == 0 && join_with_timeout(mqtt_thread,     "mqtt",     2) != 0)
        all_joined = 0;

    /*
     * 有线程没能在超时内退出：**不能**做资源清理。
     *
     * 那个线程可能正拿着 g_i2c_fd 做 ioctl，或者正用着 gpiod 的 line。
     * 这时候 close() 掉 fd，线程下一次 ioctl 就打到别的对象上了（fd 号
     * 会被系统复用），max30102_int_cleanup() 也会把它的 line 抽走 ——
     * 轻则报错，重则变成一个极难复现的踩内存 bug。
     *
     * 正确做法是直接 _exit()，让内核一次性把整个进程的地盘回收掉，
     * 谁也别想在别人脚下抽东西。代价是跳过清理，但退出阶段这点代价
     * 比"清一半"安全得多。
     */
    if (!all_joined) {
        fprintf(stderr,
                "[WARN] 有线程未在超时内退出，跳过资源清理直接退出\n");
        show_cursor();
        /*
         * 这里**故意**不调 wd_stop()：
         * 既然有线程卡着没收掉，这次退出就不是"正常收尾"，统计文件里的
         * last_clean 应该保持 0 —— 让下次启动把它记成一次异常退出。
         * 至于看门狗本身：_exit() 之后由内核收尾，但只要有线程还卡在 D 状态，
         * 它占着的那份 files_struct 引用就放不掉，/dev/watchdog 不会被关闭，
         * 于是没人喂狗 —— 到点硬件复位整板。这正是我们要的兜底。
         */
        _exit(EXIT_FAILURE);
    }

    /* 清理资源 */
    max30102_int_cleanup();
    show_cursor();
    if(g_i2c_fd >= 0)
        i2c_close(g_i2c_fd);

    /*
     * 正常收尾完成后才关看门狗设备。这一步会：
     *   1. 把 last_clean = 1 写进统计文件（下次启动就知道这次是干净退的）；
     *   2. magic close，把这台设备的喂狗责任交还给内核（imx2_wdt 没有 .stop，
     *      硬件停不了，但内核会接手补喂，所以板子不会被误复位）。
     * 必须放在所有线程都收掉之后 —— 早关就等于收尾期间没人保底。
     */
    wd_stop();

    return EXIT_SUCCESS;
}
