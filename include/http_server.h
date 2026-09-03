/**
 * @file    http_server.h
 * @brief   健康监测终端 — HTTP 服务端模块
 *
 * 基于 libmicrohttpd，提供：
 *   GET /            → HTML 仪表盘页面
 *   GET /api/sensors → 传感器 JSON 数据
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* thread_http_server — HTTP 服务端线程入口 */
void *thread_http_server(void *arg);

#endif /* HTTP_SERVER_H */
