/**
 * @file    display.h
 * @brief   健康监测终端 — 终端显示模块
 *
 * 统一渲染传感器数据和 TCP/HTTP/MQTT 日志到终端。
 * 所有输出由本模块的线程统一执行，避免多线程 printf 争抢 stdout。
 */

#ifndef DISPLAY_H
#define DISPLAY_H

/* thread_display — 终端显示线程入口 */
void *thread_display(void *arg);

#endif /* DISPLAY_H */
