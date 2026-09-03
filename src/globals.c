/**
 * @file    globals.c
 * @brief   健康监测终端 — 共享全局变量定义 + 工具函数实现
 */

#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>

/* ═══ 全局句柄 ═══ */
int g_i2c_fd = -1;

/* ═══ 传感器共享数据 ═══ */
PPG_Result_t       g_latest_ppg     = {0};
mpu6050_raw_data_t g_latest_mpu_raw = {0};
mpu6050_data_t     g_latest_mpu     = {0};
int                g_ppg_fill       = 0;
pthread_mutex_t    g_ppg_lock       = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t    g_mpu_lock       = PTHREAD_MUTEX_INITIALIZER;

/* ═══ 运行状态标志位 ═══ */
volatile sig_atomic_t g_running         = 1;
volatile sig_atomic_t g_mqtt_connected  = 0;

/* ═══ TCP 服务端共享数据 ═══ */
int  clients[MAX_CLIENTS];
int  client_count       = 0;
char read_buf[BUF_SIZE];
char write_buf[BUF_SIZE];

/* ═══ TCP 日志环形缓冲 ═══ */
char tcp_log_buf[TCP_LOG_LINES][TCP_LOG_MAXLEN];
int  tcp_log_head       = 0;
int  tcp_log_count      = 0;
pthread_mutex_t tcp_log_lock = PTHREAD_MUTEX_INITIALIZER;

/* ─── 终端控制 ─── */
void clear_screen(void) { printf("\033[2J\033[H"); }
void hide_cursor(void)  { printf("\033[?25l"); fflush(stdout); }
void show_cursor(void)  { printf("\033[?25h"); fflush(stdout); }

/* ─── 获取系统毫秒时间戳（单调时钟，不受系统时间调整影响） ─── */
uint64_t get_sys_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ─── TCP 日志：线程安全地写入环形缓冲 ─── */
void tcp_log(const char *fmt, ...)
{
    va_list ap;
    char msg[TCP_LOG_MAXLEN];
    size_t len;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 去掉末尾换行符（显示时统一处理） */
    len = strlen(msg);
    while (len > 0 && (msg[len-1] == '\n' || msg[len-1] == '\r'))
        msg[--len] = '\0';

    pthread_mutex_lock(&tcp_log_lock);
    strncpy(tcp_log_buf[tcp_log_head], msg, TCP_LOG_MAXLEN - 1);
    tcp_log_buf[tcp_log_head][TCP_LOG_MAXLEN - 1] = '\0';
    tcp_log_head = (tcp_log_head + 1) % TCP_LOG_LINES;
    if (tcp_log_count < TCP_LOG_LINES)
        tcp_log_count++;
    pthread_mutex_unlock(&tcp_log_lock);
}

/* ─── 信号处理器 ─── */
/*
 * 多线程环境下，SIGINT/SIGTERM 由内核投递给任意一个线程。
 * 这里只置 g_running = 0，各线程在自己的循环中检查此标志位后退出。
 */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ─── 信号注册 ─── */
/*
 * 必须用 sigaction 而非 signal()。
 *
 * Linux glibc 中 signal() 封装的 sigaction 默认包含 SA_RESTART 标志，
 * 会导致阻塞中的系统调用（poll、epoll_wait、usleep）被信号中断后
 * 自动重试，而非返回 EINTR。结果：Ctrl+C 后 g_running=0 置位了，
 * 但线程仍卡在系统调用上永远不退出。
 *
 * sa_flags = 0 显式关闭 SA_RESTART，保证信号处理函数执行后
 * 系统调用返回 EINTR，线程检查 g_running 后正常退出。
 */
int setup_signals(void)
{
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            /* 不加 SA_RESTART */

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction(SIGINT)");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction(SIGTERM)");
        return -1;
    }
    return 0;
}
