/**
 * @file    mqtt_client.c
 * @brief   健康监测终端 — MQTT 客户端实现（本地/云端双模式）
 *
 * 本地模式：连接 localhost:1883（板端自建 mosquitto broker）
 * 云端模式：连接华为云 IoTDA MQTT broker（TLS 8883 端口 + HMAC-SHA256 鉴权）
 *
 * 主循环：
 *   while (running):
 *       mosquitto_loop(100ms)   ← 驱动网络 I/O
 *       每 500ms → 读传感器 → publish("health/sensors", JSON)
 *                           → publish("$oc/devices/.../properties/report", JSON) [仅云端]
 */

#include "mqtt_client.h"
#include "globals.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mosquitto.h>      /* libmosquitto — MQTT 客户端 */

#ifdef MQTT_CLOUD_MODE
#include <openssl/hmac.h>   /* OpenSSL — HMAC-SHA256（华为云 MQTT 鉴权签名） */
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  华为云 IoTDA MQTT 鉴权辅助函数（仅在 MQTT_CLOUD_MODE 下编译）
 *
 *  按华为云设备接入标准，计算 MQTT 连接所需的三要素：
 *    1. Broker 域名       — 从控制台直接复制（每设备独立域名）
 *    2. ClientID          — 含设备标识 + 签名方式 + 时间戳
 *    3. Password          — HMAC-SHA256(key=时间戳, data=DeviceSecret)
 *
 *  时间戳格式为 YYYYMMDDHH（UTC），密码每小时过期，需板端时间准确。
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef MQTT_CLOUD_MODE

/*
 * 获取当前 UTC 时间，格式化为华为云要求的 YYYYMMDDHH（年月日时）。
 *
 * 例：2026-07-21 14:30:00 UTC → "2026072114"
 *
 * 这是华为云 IoTDA MQTT 鉴权的时间戳格式，密码每小时变化一次，
 * 因此板端时间须与 NTP 同步（偏差不超过 1 小时即可通过鉴权）。
 */
static void get_huawei_timestamp(char *ts_out, size_t ts_size)
{
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    strftime(ts_out, ts_size, "%Y%m%d%H", utc);
}

/*
 * 构建华为云 IoTDA 的 MQTT ClientID。
 *
 * 格式：{ProductKey}_{DeviceName}_0_0_{YYYYMMDDHH}
 *
 * 各部分含义：
 *   第1段 — 设备标识（ProductKey_DeviceName）
 *   第2段 — 认证类型（0 = 设备 ID/密钥方式）
 *   第3段 — 签名方式（0 = HMACSHA256，不校验时间戳）
 *   第4段 — 时间戳（与 Password 计算使用的时间戳相同）
 */
static void build_huawei_client_id(char *client_id_out, size_t ci_size,
                                    const char *timestamp)
{
    snprintf(client_id_out, ci_size, "%s_%s_0_0_%s",
             MQTT_CLOUD_PRODUCT_KEY, MQTT_CLOUD_DEVICE_NAME, timestamp);
}

/*
 * 计算华为云 IoTDA 的 MQTT Password。
 *
 * 算法：HMAC-SHA256(key = 时间戳_YYYYMMDDHH, data = DeviceSecret)
 * 输出：64 字符十六进制小写字符串
 *
 * OpenSSL HMAC() 函数参数：
 *   evp_md   — 哈希算法（EVP_sha256() 返回 SHA-256 描述符）
 *   key      — HMAC 密钥（时间戳字符串 "YYYYMMDDHH"，10 字节）
 *   key_len  — 密钥长度
 *   d        — 待哈希数据（DeviceSecret 字符串）
 *   n        — 数据长度
 *   md       — 输出缓冲区（32 字节原始二进制）
 *   md_len   — 输出长度（SHA-256 固定返回 32）
 *
 * 然后将 32 字节二进制转为 64 字符十六进制字符串。
 */
static int compute_huawei_password(const char *device_secret,
                                    const char *timestamp,
                                    char *password_out, size_t pw_size)
{
    if (pw_size < 65) {
        return -1;  /* 64 hex + null terminator */
    }

    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;

    /*
     * HMAC() 参数：
     *   key  = 时间戳 YYYYMMDDHH（ASCII 字符串，10 字节）
     *   data = DeviceSecret（ASCII 字符串）
     *
     * 返回 NULL 表示失败（极少发生，通常是 evp_md 参数无效）。
     * 成功时 result 写入 32 字节原始 HMAC 值，result_len 被设为 32。
     */
    unsigned char *ret = HMAC(
        EVP_sha256(),                           /* 哈希算法 */
        timestamp,      (int)strlen(timestamp), /* 密钥 = 时间戳 */
        (const unsigned char *)device_secret,   /* 数据 = DeviceSecret */
        strlen(device_secret),
        result, &result_len
    );
    if (ret == NULL) {
        return -2;
    }

    /* 二进制 → 十六进制字符串 */
    for (unsigned int i = 0; i < result_len; i++) {
        snprintf(password_out + (i * 2), pw_size - (i * 2),
                 "%02x", result[i]);
    }
    password_out[result_len * 2] = '\0';

    return 0;
}

#endif /* MQTT_CLOUD_MODE */


/* ═══════════════════════════════════════════════════════════════════
 *  MQTT 回调函数
 *
 *  libmosquitto 的事件通知机制：当连接/断开发生时，库内部调用这些
 *  回调函数。我们利用它们更新连接状态标志位，并通过 tcp_log() 在
 *  终端上打印状态变化。
 *
 *  回调签名的 void *obj 是 mosquitto_new() 的第三个参数，
 *  这里没有使用，填了 NULL。
 * ═══════════════════════════════════════════════════════════════════ */

static void on_mqtt_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;

    if (rc == 0) {
        g_mqtt_connected = 1;
        tcp_log("[MQTT] 已连接 broker %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

        /*
         * 连接成功后立即发布 "online" 状态（retain=true）。
         *
         * retain 的作用：broker 保留此消息，后续新订阅 health/status
         * 的客户端会立即收到 "online"，无需等待下一次发布。
         * 遗嘱消息是 "offline"，正常在线期间 broker 保留的是本条 "online"。
         */
        mosquitto_publish(mosq, NULL, MQTT_TOPIC_STATUS,
                          (int)strlen("online"), "online",
                          /* qos */ 0, /* retain */ true);
    } else {
        g_mqtt_connected = 0;
        /* mosquitto_connack_string() 将返回码转为可读字符串 */
        tcp_log("[MQTT] 连接失败: %s", mosquitto_connack_string(rc));
    }
}

static void on_mqtt_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;

    g_mqtt_connected = 0;
    if (rc == 0) {
        tcp_log("[MQTT] 正常断开连接");
    } else {
        tcp_log("[MQTT] 意外断连 (rc=%d)", rc);
    }
}


/* ═══════════════════════════════════════════════════════════════════
 *  MQTT 客户端线程
 *
 *  支持两种模式，通过 MQTT_CLOUD_MODE 宏切换：
 *
 *  【本地模式】（默认）
 *    连接板端自建 mosquitto broker（localhost:1883），不加密，无需认证。
 *    用法：先执行 mosquitto -d 启动 broker，再运行本程序。
 *
 *  【云端模式】（取消 MQTT_CLOUD_MODE 注释启用）
 *    连接华为云 IoTDA MQTT broker（8883 端口，TLS 加密），使用三元组
 *    + HMAC-SHA256 签名鉴权。Topic 格式与本地模式相同（health/sensors、
 *    health/status），无需修改订阅端。
 *
 *    鉴权流程（每次连接时执行一次）：
 *      1. 获取当前 UTC 时间 → YYYYMMDDHH 格式
 *      2. 拼接 ClientID / Username
 *      3. HMAC-SHA256(key=时间戳, data=DeviceSecret) → Password
 *      4. mosquitto_tls_set() 加载 CA 证书
 *      5. mosquitto_username_pw_set() 设置用户名/密码
 *      6. mosquitto_connect() 发起 TLS 连接
 *
 *  无论哪种模式，主循环逻辑相同：
 *    while (running):
 *        mosquitto_loop(100ms)   ← 驱动网络 I/O
 *        每 500ms → 读传感器 → mosquitto_publish("health/sensors", JSON)
 * ═══════════════════════════════════════════════════════════════════ */

void *thread_mqtt(void *arg)
{
    (void)arg;

    int rc;
    uint64_t last_publish = 0;

    /* 初始化 libmosquitto 全局状态（所有线程只调用一次） */
    rc = mosquitto_lib_init();
    if (rc != MOSQ_ERR_SUCCESS) {
        tcp_log("[MQTT] 库初始化失败: %s", mosquitto_strerror(rc));
        return NULL;
    }

#ifdef MQTT_CLOUD_MODE
    /* ──────────── 云端模式：准备鉴权参数 ──────────── */

    char timestamp[11];  /* "YYYYMMDDHH" + '\0' */
    get_huawei_timestamp(timestamp, sizeof(timestamp));

    /* 拼接 ClientID */
    char client_id_str[128];
    build_huawei_client_id(client_id_str, sizeof(client_id_str), timestamp);

    /* 计算 Password：HMAC-SHA256(key=时间戳, data=DeviceSecret) */
    char password[65];
    rc = compute_huawei_password(MQTT_CLOUD_DEVICE_SECRET, timestamp,
                                  password, sizeof(password));
    if (rc != 0) {
        tcp_log("[MQTT] Password 计算失败 (rc=%d)", rc);
        mosquitto_lib_cleanup();
        return NULL;
    }

    /* 拼接 Username（= ProductKey_DeviceName） */
    char username[128];
    snprintf(username, sizeof(username), "%s_%s",
             MQTT_CLOUD_PRODUCT_KEY, MQTT_CLOUD_DEVICE_NAME);

    /* 创建客户端（cloud mode 下 ClientID 按华为云格式） */
    struct mosquitto *mosq = mosquitto_new(client_id_str, true, NULL);

    tcp_log("[MQTT] 云端模式 | broker: %s:%d",
            MQTT_CLOUD_BROKER, MQTT_CLOUD_PORT);
    tcp_log("[MQTT] DeviceId: %s", username);
#else
    /* ──────────── 本地模式：直接连接 ──────────── */

    struct mosquitto *mosq = mosquitto_new("health_monitor_i.MX6ULL", true, NULL);
#endif

    if (mosq == NULL) {
        tcp_log("[MQTT] 客户端创建失败");
        mosquitto_lib_cleanup();
        return NULL;
    }

    /* 注册回调（本地/云端共用） */
    mosquitto_connect_callback_set(mosq, on_mqtt_connect);
    mosquitto_disconnect_callback_set(mosq, on_mqtt_disconnect);

    /* 设置遗嘱消息（本地/云端共用） */
    mosquitto_will_set(mosq, MQTT_TOPIC_STATUS,
                       (int)strlen("offline"), "offline", 0, true);

#ifdef MQTT_CLOUD_MODE
    /*
     * TLS 配置 — 仅在云端模式启用。
     *
     * mosquitto_tls_set() 参数：
     *   cafile   — CA 证书文件路径（用于验证服务器证书）
     *   capath   — CA 证书目录（与 cafile 二选一即可）
     *   certfile — 客户端证书（华为云 IoT 不需要双向认证，传 NULL）
     *   keyfile  — 客户端私钥（同上）
     *   pw_cb    — 私钥密码回调（同上，传 NULL）
     *
     * 设置 TLS 后，mosquitto_connect() 内部自动升级为 MQTTS。
     */
    rc = mosquitto_tls_set(mosq, MQTT_TLS_CAFILE, NULL, NULL, NULL, NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        tcp_log("[MQTT] TLS 配置失败: %s", mosquitto_strerror(rc));
        tcp_log("[MQTT] 请确认 CA 证书文件存在: %s", MQTT_TLS_CAFILE);
        tcp_log("[MQTT] 安装: sudo apt install ca-certificates");
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return NULL;
    }

    /* 设置用户名和密码（华为云鉴权） */
    rc = mosquitto_username_pw_set(mosq, username, password);
    if (rc != MOSQ_ERR_SUCCESS) {
        tcp_log("[MQTT] 用户名/密码设置失败: %s", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return NULL;
    }

    /* 连接云端 broker（TLS 8883 端口） */
    rc = mosquitto_connect(mosq, MQTT_CLOUD_BROKER, MQTT_CLOUD_PORT, /* keepalive */ 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        tcp_log("[MQTT] 云端连接请求失败: %s", mosquitto_strerror(rc));
        tcp_log("[MQTT] 请检查: 1)网络连通 2)三元组正确 3)板端时间准确");
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return NULL;
    }
#else
    /* 连接本地 broker（TCP 1883 端口） */
    rc = mosquitto_connect(mosq, MQTT_BROKER_HOST, MQTT_BROKER_PORT,
                           /* keepalive */ 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        tcp_log("[MQTT] 连接请求失败: %s", mosquitto_strerror(rc));
        tcp_log("[MQTT] 请确认 broker 已启动: mosquitto -d");
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return NULL;
    }
#endif

    /*
     * 主循环：驱动 MQTT 网络 I/O + 定期发布传感器数据。
     *
     * 本地和云端模式的循环逻辑完全相同：
     *   mosquitto_loop(100ms) → 驱动 TLS/TCP 网络 I/O
     *   每 500ms → 读传感器 → publish JSON
     *
     * mosquitto_loop 的超时保证 Ctrl+C 响应延迟 ≤ 100ms。
     */
    while (g_running) {
        mosquitto_loop(mosq, /* timeout_ms */ 100, /* max_packets */ 1);

        uint64_t now = get_sys_ms();
        if (g_mqtt_connected && (now - last_publish >= MQTT_PUBLISH_INTERVAL_MS)) {
            pthread_mutex_lock(&g_mpu_lock);
            mpu6050_data_t mpu = g_latest_mpu;
            pthread_mutex_unlock(&g_mpu_lock);

            pthread_mutex_lock(&g_ppg_lock);
            PPG_Result_t ppg = g_latest_ppg;
            pthread_mutex_unlock(&g_ppg_lock);

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t timestamp_ms = (uint64_t)ts.tv_sec * 1000
                                   + ts.tv_nsec / 1000000;

            char json[512];
            int len = snprintf(json, sizeof(json),
                "{\"t\":%llu,\"mpu\":{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp\":%.1f},"
                "\"ppg\":{\"hr\":%d,\"spo2\":%d,\"valid\":%d}}",
                (unsigned long long)timestamp_ms,
                mpu.accel_x_g, mpu.accel_y_g, mpu.accel_z_g,
                mpu.gyro_x_dps, mpu.gyro_y_dps, mpu.gyro_z_dps, mpu.temp_c,
                ppg.heart_rate, ppg.spo2, ppg.data_valid
            );

            mosquitto_publish(mosq, NULL, MQTT_TOPIC_SENSORS,
                              len, json, /* qos */ 0, /* retain */ false);

#ifdef MQTT_CLOUD_MODE
            /*
             * 华为云 IoTDA 属性上报。
             *
             * 格式：
             *   Topic: $oc/devices/{device_id}/sys/properties/report
             *   Payload: {"services":[{"service_id":"xxx","properties":{...}}]}
             *
             * 注意 hr/spo2 是整数，ax 是浮点（保留 2 位小数）。
             */
            {
                char cloud_json[256];
                int cloud_len = snprintf(cloud_json, sizeof(cloud_json),
                    "{\"services\":["
                    "{\"service_id\":\"%s\","
                    "\"properties\":{"
                    "\"hr\":%d,"
                    "\"spo2\":%d,"
                    "\"ax\":%.2f"
                    "}}"
                    "]}",
                    MQTT_CLOUD_SERVICE_ID,
                    ppg.heart_rate, ppg.spo2,
                    mpu.accel_x_g
                );
                mosquitto_publish(mosq, NULL, MQTT_TOPIC_PROPERTIES,
                                  cloud_len, cloud_json,
                                  /* qos */ 0, /* retain */ false);
            }
#endif

            last_publish = now;
        }
    }

    /* 清理：发布离线状态 → 断开连接 → 释放资源 */
    if (g_mqtt_connected) {
        mosquitto_publish(mosq, NULL, MQTT_TOPIC_STATUS,
                          (int)strlen("offline"), "offline",
                          /* qos */ 0, /* retain */ true);
        tcp_log("[MQTT] 已发布离线状态");
    }

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    tcp_log("[MQTT] 线程已退出");

    return NULL;
}
