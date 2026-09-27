/**
 * @file    watchdog.h
 * @brief   硬件看门狗 + 复位计数 —— 无人值守运行的最后一道防线
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

/*
 * 需要被监视的线程槽位。
 *
 * 入选标准：它的主循环里每一步都有明确时间上界 —— 要么是定时 sleep，
 * 要么是带超时的 poll/epoll_wait，要么是"再慢也一定会返回"的单次内核调用。
 *
 * 故意**不**放 mqtt：mosquitto_connect() 是阻塞调用（DNS + TCP + TLS 握手 +
 * 等 CONNACK），云端断网时一口气卡几十秒是正常的、而且它自己会恢复。
 * 把它算进健康集合，等于让一次网络抖动变成一次整板重启 —— 那是捣乱，
 * 不是保护。等以后真需要盯它，也应该单开一档更宽的时间窗。
 */
enum {
    WD_CH_MAX30102 = 0,   /* MAX30102 采集线程（poll + I2C） */
    WD_CH_MPU6050,        /* MPU6050 采集线程（usleep + I2C） */
    WD_CH_DISPLAY,        /* 终端显示线程 */
    WD_CH_TCP,            /* TCP 服务端线程（epoll） */
    WD_CH_HTTP,           /* HTTP 服务端线程（libmicrohttpd） */
    WD_CH_COUNT
};

/*
 * 打开看门狗并完成本次启动的计数。
 * 返回 0 表示看门狗已生效；返回 -1 表示设备不可用（本次运行没有硬件保护，
 * 但不影响程序其它功能，调用方不需要当致命错误处理）。
 */
int wd_init(void);

/* 线程心跳。各线程在自己循环里调，只写自己那个槽位。 */
void wd_beat(int ch);

/* 正常退出前调用：写统计文件 + magic close 交还给内核补喂 */
void wd_stop(void);

/* 巡检线程：任一被监视线程停跳超时 → 停止喂狗 → 硬件复位整板 */
void *thread_watchdog(void *arg);

#endif /* WATCHDOG_H */
