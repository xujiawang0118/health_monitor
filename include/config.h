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
 * _POSIX_C_SOURCE 199309L → clock_gettime / CLOCK_MONOTONIC
 * _DEFAULT_SOURCE          → usleep（POSIX.1-2008 已废弃，需显式开启）
 *
 * 较新 glibc（2.40+）已预定义 _POSIX_C_SOURCE，此处仅做最小版本保证。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#define _DEFAULT_SOURCE

/* ─── 传感器 / 显示参数 ─── */
#define I2C_BUS_NUM            0       /* /dev/i2c-0（两个传感器共用） */
#define MPU6050_INTERVAL_MS    50      /* MPU6050 读取间隔 50ms → 20Hz */
#define DISPLAY_INTERVAL_MS    500     /* 终端刷新间隔 500ms → 2Hz */
#define POLL_TIMEOUT_MS        50      /* poll 超时（让 MPU6050 能独立节奏运行） */

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

#define MQTT_PUBLISH_INTERVAL_MS 500        /* MQTT 发布间隔（2Hz，与 TCP 广播对齐） */
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

#endif /* CONFIG_H */
