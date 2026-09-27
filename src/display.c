/**
 * @file    display.c
 * @brief   健康监测终端 — 终端显示实现
 *
 * 每隔 DISPLAY_INTERVAL_MS 毫秒刷新终端，包括：
 *   - 传感器原始数据和物理量
 *   - TCP/HTTP/MQTT 服务端日志（来自 tcp_log 环形缓冲）
 *
 * 所有输出由本线程统一渲染，避免多线程 printf 争抢 stdout
 * 造成输出混叠。各工作线程只向 tcp_log_buf 写入消息。
 */

#include "display.h"
#include "globals.h"
#include "watchdog.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>


/* ─── 格式化传感器数据显示 ─── */
/*
 * 静态缓冲区，单线程安全（仅由 thread_display 调用）。
 */
static const char *format_display(const mpu6050_data_t *mpu_d,
                                   const mpu6050_raw_data_t *mpu_r,
                                   const PPG_Result_t *ppg,
                                   int ppg_window_fill)
{
    static char buf[1536];

    /*
     * 排版说明：
     *   \033[K → 清除从光标到行尾的内容（避免残留字符）
     *
     * MPU6050 部分：加速度/陀螺仪/温度
     * MAX30102 部分：心率/血氧 + 窗口填充进度
     * 底部状态栏：刷新间隔 + 退出提示
     */
    snprintf(buf, sizeof(buf),
             /* ── MPU6050 ── */
             "=== MPU6050 ===\033[K\n"
             " Raw(LSB):  AX:%+6d  AY:%+6d  AZ:%+6d  T:%+6d\033[K\n"
             "            GX:%+6d  GY:%+6d  GZ:%+6d\033[K\n"
             " Physical:  Acc(g)  X=%+8.2f  Y=%+8.2f  Z=%+8.2f\033[K\n"
             "            Gyro    X=%+8.2f  Y=%+8.2f  Z=%+8.2f\033[K\n"
             "            Temp  %6.2f C\033[K\n"
             "\n"
             /* ── MAX30102 ── */
             "=== MAX30102 ===\033[K\n"
             " PPG window: %d/%d samples\033[K\n",
             mpu_r->accel_x, mpu_r->accel_y, mpu_r->accel_z, mpu_r->temp,
             mpu_r->gyro_x,  mpu_r->gyro_y,  mpu_r->gyro_z,
             mpu_d->accel_x_g, mpu_d->accel_y_g, mpu_d->accel_z_g,
             mpu_d->gyro_x_dps, mpu_d->gyro_y_dps, mpu_d->gyro_z_dps,
             mpu_d->temp_c,
             ppg_window_fill, PPG_WINDOW_SIZE);

    /* PPG 结果追加到缓冲区 */
    int written = strlen(buf);

    if (ppg != NULL && ppg->data_valid) {
        snprintf(buf + written, sizeof(buf) - written,
                 " Heart Rate: %3u bpm    SpO2: %3u %%    [valid]\033[K\n",
                 ppg->heart_rate, ppg->spo2);
    } else if (ppg != NULL && !ppg->data_valid) {
        snprintf(buf + written, sizeof(buf) - written,
                 " Heart Rate: --- bpm    SpO2: --- %%    [waiting]\033[K\n");
    } else {
        snprintf(buf + written, sizeof(buf) - written,
                 " Heart Rate: --- bpm    SpO2: --- %%    [no data]\033[K\n");
    }

    /* 状态栏 */
    written = strlen(buf);
    snprintf(buf + written, sizeof(buf) - written,
             "\nCtrl+C to exit\033[K");

    return buf;
}


/* ═══════════════════════════════════════════════════════════════════
 *  显示线程
 *
 *  每隔 DISPLAY_INTERVAL_MS 毫秒刷新终端显示，包括：
 *    - 传感器原始数据和物理量
 *    - TCP/HTTP/MQTT 服务端日志（来自 tcp_log 环形缓冲）
 *
 *  所有输出由本线程统一渲染，避免多线程 printf 争抢 stdout
 *  造成输出混叠。TCP/HTTP/MQTT 线程只向 tcp_log_buf 写入消息。
 * ═══════════════════════════════════════════════════════════════════ */
void *thread_display(void *arg)
{
    (void)arg;
    while (g_running) {
        wd_beat(WD_CH_DISPLAY);

        usleep(DISPLAY_INTERVAL_MS * 1000);

        pthread_mutex_lock(&g_ppg_lock);
        PPG_Result_t ppg = g_latest_ppg;
        int ppg_fill = g_ppg_fill;
        pthread_mutex_unlock(&g_ppg_lock);

        pthread_mutex_lock(&g_mpu_lock);
        mpu6050_data_t mpu = g_latest_mpu;
        mpu6050_raw_data_t mpu_raw = g_latest_mpu_raw;
        pthread_mutex_unlock(&g_mpu_lock);

        /*
         * \033[H  → 光标移到左上角
         * \033[J  → 清除从光标到屏幕末尾（避免旧残影）
         *
         * 传感器数据 + TCP 日志区域由同一个线程渲染，消除多线程
         * printf 造成的输出混叠。TCP 日志来自 tcp_log_buf 环形缓冲，
         * TCP 线程只写缓冲，显示线程只读缓冲（通过 tcp_log_lock 保护）。
         */
        printf("\033[H\033[J%s", format_display(&mpu, &mpu_raw, &ppg, ppg_fill));

        /* ── 服务端日志区域 ── */
        printf("\n--- MQTT :%d | HTTP :%d | TCP :6666 ---\033[K\n",
               MQTT_BROKER_PORT, HTTP_PORT);

        pthread_mutex_lock(&tcp_log_lock);
        {
            int total = tcp_log_count;
            if (total == 0) {
                printf("  (等待客户端连接...)\033[K\n");
            } else {
                /*
                 * 环形缓冲遍历：
                 *   total < TCP_LOG_LINES → 条目在 [0, total-1]
                 *   total == TCP_LOG_LINES → 最早在 tcp_log_head，绕回
                 */
                int start = (total < TCP_LOG_LINES) ? 0 : tcp_log_head;
                for (int i = 0; i < total; i++) {
                    int idx = (start + i) % TCP_LOG_LINES;
                    printf("  %s\033[K\n", tcp_log_buf[idx]);
                }
            }
        }
        pthread_mutex_unlock(&tcp_log_lock);

        fflush(stdout);
    }
    return NULL;
}
