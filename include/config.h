/**
 * @file    config.h
 * @brief   健康监测终端 — 全局配置
 *
 * 所有可调参数集中在这里，各模块 include 此文件即可获取配置值。
 */

#ifndef CONFIG_H
#define CONFIG_H

/*
 * 特性宏 — 必须在所有系统头文件之前定义。
 *
 * _GNU_SOURCE              → pthread_timedjoin_np 等 GNU 扩展（带超时的 join）
 * _POSIX_C_SOURCE 199309L → clock_gettime / CLOCK_MONOTONIC
 * _DEFAULT_SOURCE          → usleep（POSIX.1-2008 已废弃，需显式开启）
 *
 * 较新 glibc（2.40+）已预定义 _POSIX_C_SOURCE，此处仅做最小版本保证。
 * 注意：-std=gnu11 不会自动定义 _GNU_SOURCE，必须显式定义，
 * 才能用 pthread_timedjoin_np 这类带 _GNU_SOURCE 保护的接口。
 */
#define _GNU_SOURCE
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#define _DEFAULT_SOURCE

/* 传感器 mock：PC 端无硬件时启用，用假数据替代真实 I2C/GPIO 采集 */
// #define SENSOR_MOCK
    

/* ─── 传感器 / 显示参数 ─── */
#define I2C_BUS_NUM            0       /* /dev/i2c-0（两个传感器共用） */
#define MPU6050_INTERVAL_MS    50      /* MPU6050 读取间隔 50ms → 20Hz */
#define DISPLAY_INTERVAL_MS    500     /* 终端刷新间隔 500ms → 2Hz */
#define POLL_TIMEOUT_MS        50      /* poll 超时（让 MPU6050 能独立节奏运行） */
#define MPU6050_FAIL_RESET_THRESHOLD  3  /* 连续读失败/读全0 N 次后软复位 MPU6050 */

/* ─── HTTP 服务端口 ─── */
#define HTTP_PORT               8080

/* ─── TCP 日志环形缓冲 ─── */
#define TCP_LOG_LINES  6
#define TCP_LOG_MAXLEN 128

/* ============================================================
 *  MQTT 配置 — 本地 / 云端二选一
 * ============================================================
 *
 * 本地模式（默认）：
 *   板端自建 mosquitto broker（mosquitto -d），不加密，无需认证。
 *   适合本地调试和局域网内数据分发。
 *
 * 云端模式：
 *   取消下面 MQTT_CLOUD_MODE 的注释，并填入华为云 IoTDA 控制台获取的
 *   三元组（ProductKey / DeviceName / DeviceSecret）和设备接入地址。
 *
 *   华为云控制台 → 设备详情 → 设备信息 → 接入信息：
 *     Broker   直接复制控制台上的"设备接入地址"（每设备独立域名）
 *     Port     8883（MQTTS）
 *     ClientID = {ProductKey}_{DeviceName}_0_0_{YYYYMMDDHH}
 *               （_0_0_ = auth_type=设备ID, sign_method=HMACSHA256不校验时间戳）
 *     Username = {ProductKey}_{DeviceName}
 *     Password = HMAC-SHA256(key=时间戳_YYYYMMDDHH, data=DeviceSecret) → 64 hex
 *
 *   时间戳格式为 UTC 时间的 YYYYMMDDHH（年月日时），密码每小时变化一次，
 *   板端时间必须准确（建议开启 NTP 对时）。
 *
 * 注意：
 *   - 云端模式需要 libssl-dev（OpenSSL）和根证书包（ca-certificates）
 *   - 根证书路径默认 /etc/ssl/certs/ca-certificates.crt（Debian）
 *   - 如果板端没有 ca-certificates 包，需安装：sudo apt install ca-certificates
 */

/* 取消下面这行的注释即可切换到云端模式 */
#define MQTT_CLOUD_MODE

#ifdef MQTT_CLOUD_MODE
/* ── 华为云 IoT 平台配置（三元组与接入地址在 cloud_secret.h，勿提交 git）── */
#include "cloud_secret.h"
#define MQTT_CLOUD_PORT          8883
/* ── 根证书路径 ── */
#define MQTT_TLS_CAFILE          "/etc/ssl/certs/ca-certificates.crt"
#define MQTT_BROKER_HOST         "华为云IoT"
#define MQTT_BROKER_PORT         MQTT_CLOUD_PORT
#else
/* ── 本地 broker 配置 ── */
#define MQTT_BROKER_HOST         "localhost"
#define MQTT_BROKER_PORT         1883
#endif

#define MQTT_PUBLISH_INTERVAL_MS 500        /* 本地模式发布间隔（2Hz，与 TCP 广播对齐） */

#ifdef MQTT_CLOUD_MODE
/*
 * 云端模式发布间隔。
 *
 * 华为云 IoTDA 每日消息配额有限（免费版默认 10000 条/日），云端不能像
 * 本地那样 2Hz 全量上报。每次上报 2 条消息（health/sensors + 属性上报），
 * 30s 间隔 → 86400/30 × 2 = 5760 条/日，留足余量。
 */
#define MQTT_CLOUD_PUBLISH_INTERVAL_MS 30000
#endif
#define MQTT_TOPIC_SENSORS     "health/sensors"  /* 传感器数据 topic */
#define MQTT_TOPIC_STATUS      "health/status"   /* 板端在线状态 topic（含遗嘱消息） */
/*
 * 华为云 IoTDA 属性上报 topic（仅云端模式使用）
 *
 * 格式：$oc/devices/{device_id}/sys/properties/report
 * device_id = ProductKey_DeviceName
 *
 * MQTT_CLOUD_SERVICE_ID 是你在华为云控制台定义产品模型时设置的服务 ID，
 * 控制台路径：产品 → 产品模型 → 新增服务 → 服务 ID。
 */
#ifdef MQTT_CLOUD_MODE
#define MQTT_CLOUD_SERVICE_ID  "total"
#define MQTT_TOPIC_PROPERTIES  "$oc/devices/" MQTT_CLOUD_PRODUCT_KEY \
                               "_" MQTT_CLOUD_DEVICE_NAME \
                               "/sys/properties/report"
#endif

/* ============================================================
 *  看门狗 —— 无人值守运行的最后一道防线
 * ============================================================
 *
 * 前面几层保护（协作式退出、带超时的 join、_exit 兜底）都依赖同一个前提：
 * 进程自己还跑得起来。一旦有线程陷进内核的 D 状态，信号投递不进去，
 * Ctrl+C / Ctrl+Z 全都没反应，只能拔电 —— 这时唯一还能救场的就只有
 * SoC 内部的硬件看门狗。
 *
 * 下面这些参数描述的是两件事："多快判定线程卡死"和"判死之后多久复位"：
 *
 *   心跳停滞判定 = WATCHDOG_STALE_MS          = 20 秒
 *   硬件复位     = 在那之后再等 WATCHDOG_TIMEOUT_S = 30 秒
 *   ────────────────────────────────────────────────
 *   从线程卡死到整板重启约 50 秒
 *
 * 为什么阈值要留到 20 秒这么宽：
 *   I2C 总线被从机拉死时，i2c_read_reg() 的 5 次重试里，每次都要在内核里
 *   等 i2c-imx 的 500ms 总线忙超时（i2c_imx_bus_busy 里的 schedule 自旋），
 *   再加上总线恢复的 9 个 SCL 脉冲和第二次 i2c_imx_start，单次 ioctl 最坏
 *   2~3 秒，一次读能到 10 秒以上；MAX30102 线程还要在内层 for 里连读
 *   fifo_count 个样本，一轮循环可能拖到十几秒。阈值压得比这还低，总线一
 *   抖动就把板子喂重启 —— 那不是保护，是捣乱。
 *
 *   注意停滞判定是按**真实流逝时间**算的，不是按"巡检了多少轮"。
 *   WATCHDOG_CHECK_INTERVAL_MS 只决定判定精度，不参与阈值计算。
 *
 * WATCHDOG_TIMEOUT_S 有一条硬线：**必须 <= 128**。
 * 超过之后内核（watchdog_need_worker 的第一个条件）会接替用户态补喂，
 * 看门狗彻底失效。src/watchdog.c 里有 #error 在编译期钉死这条。
 */
#define WATCHDOG_ENABLE            1
#define WATCHDOG_DEV               "/dev/watchdog"
#define WATCHDOG_TIMEOUT_S         30      /* 停止喂狗后多久复位，硬上限 128 */
#define WATCHDOG_CHECK_INTERVAL_MS 2000    /* 巡检周期（只影响判定精度） */
#define WATCHDOG_STALE_MS          20000   /* 心跳停滞多久判定卡死 */

/*
 * 跨重启统计文件的位置。
 *
 * 路径在运行时解析（见 watchdog.c 的 wd_resolve_state_dir），解析顺序：
 *   1. 环境变量 HEALTH_MONITOR_STATE_DIR（最高优先级，绝对路径）
 *   2. sudo 场景下取 SUDO_USER 的家目录 + "/demo"
 *   3. 否则取 $HOME + "/demo"
 *   4. 都拿不到就退化成当前工作目录下的 ./demo
 *
 * 解析结果会在启动时打印出来 —— 别猜文件在哪，看日志。
 *
 * 为什么不用 /var/lib：那个目录只有 root 能建，而程序平时是普通用户跑；
 * 更实际的是，这个统计文件是给人看的，放在家目录下随项目一起备份更方便。
 *
 * 目录建不出来时统计不落盘，但看门狗本身照常工作（两者互不依赖）。
 */
#define WATCHDOG_STATE_ENV        "HEALTH_MONITOR_STATE_DIR"
#define WATCHDOG_STATE_SUBDIR     "demo"
#define WATCHDOG_STATE_FILE_NAME  "watchdog_stats.conf"
#define WATCHDOG_LOG_FILE_NAME    "watchdog_boots.log"

#endif /* CONFIG_H */
