/**
 * @file    globals.h
 * @brief   健康监测终端 — 共享全局变量和工具函数声明
 *
 * 所有模块需要访问的全局变量和工具函数在此集中声明。
 * 定义在 globals.c 中。
 */

#ifndef GLOBALS_H
#define GLOBALS_H

#include "config.h"    /* 必须在系统头文件之前——_POSIX_C_SOURCE 等特性宏 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include "mpu6050.h"
#include "max30102.h"
#include "tcp_server.h"

/* ═══ 全局句柄 ═══ */
extern int g_i2c_fd;

/* ═══ 传感器共享数据（互斥锁保护） ═══ */
extern PPG_Result_t       g_latest_ppg;
extern mpu6050_raw_data_t g_latest_mpu_raw;
extern mpu6050_data_t     g_latest_mpu;
extern int                g_ppg_fill;
extern pthread_mutex_t    g_ppg_lock;
extern pthread_mutex_t    g_mpu_lock;

/* ═══ 运行状态标志位 ═══ */
extern volatile sig_atomic_t g_running;
extern volatile sig_atomic_t g_mqtt_connected;

/* ═══ TCP 服务端共享数据 ═══ */
extern int  clients[MAX_CLIENTS];
extern int  client_count;
extern char read_buf[BUF_SIZE];
extern char write_buf[BUF_SIZE];

/* ═══ TCP 日志环形缓冲 ═══ */
extern char tcp_log_buf[TCP_LOG_LINES][TCP_LOG_MAXLEN];
extern int  tcp_log_head;
extern int  tcp_log_count;
extern pthread_mutex_t tcp_log_lock;

/* ═══ 工具函数 ═══ */

/* 终端控制 */
void clear_screen(void);
void hide_cursor(void);
void show_cursor(void);

/* 获取系统单调时钟毫秒时间戳 */
uint64_t get_sys_ms(void);

/* 线程安全的日志写入（写入环形缓冲，由显示线程统一渲染） */
void tcp_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* 注册 SIGINT/SIGTERM 信号处理器 */
int setup_signals(void);

#endif /* GLOBALS_H */
