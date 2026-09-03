/**
 * @file    mqtt_client.h
 * @brief   健康监测终端 — MQTT 客户端模块
 *
 * 支持两种模式，通过 MQTT_CLOUD_MODE 宏切换：
 *   - 本地模式：连接板端自建 mosquitto broker（localhost:1883）
 *   - 云端模式：连接华为云 IoTDA（TLS + HMAC-SHA256 鉴权）
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

/* thread_mqtt — MQTT 客户端线程入口 */
void *thread_mqtt(void *arg);

#endif /* MQTT_CLIENT_H */
