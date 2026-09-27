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

/*
 * 已收到的退出信号次数，用于分级退出（详见 signal_handler 的注释）。
 * 必须是 sig_atomic_t：它可能被信号处理函数改写、被其他代码读，
 * 而 sig_atomic_t 保证单次读写是原子的，不会出现读到半个值的情况。
 */
static volatile sig_atomic_t g_sig_count = 0;

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
/*
 * 每条日志前面自动加一个 CLOCK_MONOTONIC 时间戳。
 *
 * 用 CLOCK_MONOTONIC 而不是 CLOCK_REALTIME，有两个原因：
 *
 *   1. 单调递增，不受 NTP 对时和 settimeofday 调整影响。板子跑 MQTT
 *      云端模式时会开 NTP 对时，用 CLOCK_REALTIME 会出现时间跳变甚至
 *      倒流，日志时间线就不可信了。
 *
 *   2. 它的起点是开机时刻，和内核 dmesg 的时间戳（[ 2678.623972] 就是
 *      开机后 2678.623 秒）是同一个基准。所以程序日志和 dmesg 里的内核
 *      报错（比如 Goodix-TS 的 I2C 报错）可以直接按秒数对齐比对 ——
 *      排查 I2C 总线故障时，能一眼看出是传感器先失败还是触摸屏先失败。
 *
 * 格式化后前缀形如 "[ 2678.623] "，共 12 字节，占用环形缓冲的一个位置，
 * 显示线程不需要改动（它整行照打）。
 */
void tcp_log(const char *fmt, ...)
{
    va_list ap;
    char msg[TCP_LOG_MAXLEN];
    struct timespec ts;
    int prefix;
    size_t len;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    prefix = snprintf(msg, sizeof(msg), "[%9.3f] ",
                      (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);

    /*
     * snprintf 在发生截断或编码错误时会返回"本该写入的长度"，可能 >= size。
     * 前缀固定 12 字节、永远不会截断，但这里仍做一次钳位：否则下面
     * sizeof(msg) - (size_t)prefix 会对无符号数下溢成一个巨大值，
     * msg + prefix 直接越界写。
     */
    if (prefix < 0)
        prefix = 0;
    if ((size_t)prefix >= sizeof(msg))
        prefix = (int)sizeof(msg) - 1;

    va_start(ap, fmt);
    vsnprintf(msg + prefix, sizeof(msg) - (size_t)prefix, fmt, ap);
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
 *
 * ## 为什么需要"按两次"分级退出
 *
 * 上面的协作式退出有个前提：**每个线程都必须自己检查 g_running**。
 * 现实中这个前提会破：
 *   - mosquitto_connect() 是阻塞调用，内部要做 DNS + TCP + TLS 握手 +
 *     等 CONNACK，整个过程不检查我们的标志位，可能卡几十秒；
 *   - 驱动里卡住的 ioctl 更是完全不受我们控制；
 *   - 主线程如果卡在没有超时的 pthread_join 上，同样退不出来。
 *
 * 只要命中任意一条，进程就永远退不出去，用户除了拔电没别的办法。
 * 所以留一个"再按一次一定走"的兜底：优雅重要，但能退出更重要。
 *
 * ## 为什么 handler 里不能调 tcp_log / show_cursor / exit
 *
 * 信号处理函数运行在**被中断线程的上下文**里，只能用异步信号安全的函数
 * （async-signal-safe，见 man 7 signal 的列表）：
 *
 *   write()  ✔  唯一安全的输出方式
 *   _exit()  ✔  直接进内核终结进程
 *   exit()   ✘  会跑 atexit 处理器、刷 stdio 缓冲，和主线程的 stdio
 *               操作竞争，可能死锁
 *   printf   ✘  同上，而且不可重入
 *   tcp_log  ✘  它要拿 tcp_log_lock 互斥锁。如果信号恰好打断了正持有
 *               该锁的线程，handler 里再取锁就是死锁 —— 按 Ctrl+C 反而
 *               把进程彻底锁死，比不装 handler 还糟
 *
 * 这就是下面全部用 write(2) 裸写终端的原因。
 */
static void signal_handler(int sig)
{
    (void)sig;

    /* 第二次及以后的信号：强制退出，不再等待任何线程收尾 */
    if (++g_sig_count >= 2) {
        static const char msg[] = "\n[强制退出]\033[?25h\n";
        ssize_t n = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        (void)n;
        /* \033[?25h 顺手把光标显示回来 —— 正常路径是 show_cursor() 干的，
         * 强制退出时跳过了它，不清的话终端会一直没光标 */
        _exit(EXIT_FAILURE);
    }

    /* 第一次信号：走正常收尾流程（光标等收尾完了由 show_cursor() 恢复） */
    static const char msg[] =
        "\n[收到退出信号] 正在收尾，再按一次 Ctrl+C 强制退出\n";
    ssize_t n = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)n;
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
    

    /* 忽略 SIGPIPE：客户端异常断开时 send() 返回 -1/EPIPE，而非终止进程 */
    struct sigaction sa_pipe;
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, NULL);

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
