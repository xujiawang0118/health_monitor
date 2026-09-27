/**
 * @file    http_server.c
 * @brief   健康监测终端 — HTTP 服务端实现
 *
 * 基于 libmicrohttpd 的轻量 HTTP 服务端，提供：
 *   - 嵌入式 HTML 仪表盘（Chart.js 实时波形）
 *   - /api/sensors JSON API
 */

#include "http_server.h"
#include "globals.h"
#include "watchdog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <microhttpd.h>

/* libmicrohttpd 0.9.71 起，访问回调的返回类型从 int 改成了 enum MHD_Result。
 * PC 端（1.0.x）有 MHD_Result；板端 Debian 10 是 0.9.62，只有 int。
 * 用 MHD_VERSION 版本宏做兼容，让两边回调类型都能匹配 MHD_AccessHandlerCallback。
 *   MHD_VERSION 编码：(major<<24)|(minor<<16)|(patch<<8)，0.9.71 == 0x00094700。
 */
#if MHD_VERSION >= 0x00094700
typedef enum MHD_Result mhd_result_t;
#else
typedef int mhd_result_t;
#endif


/* ═══════════════════════════════════════════════════════════════════
 *  HTML 仪表盘页面（编译时嵌入，不依赖外部文件）
 *
 *  页面通过 Chart.js CDN 渲染两组实时波形：
 *    左图 — MPU6050 六轴（加速度 X/Y/Z + 角速度 X/Y/Z），6 条线
 *    右图 — MAX30102 心率 + 血氧，2 条线
 *
 *  数据获取方式：浏览器每 500ms 调用 fetch("/api/sensors") 拉 JSON，
 *  然后追加到 Chart.js 的数据队列。不使用 SSE 以避免增加协议复杂度。
 *
 *  R"HTML(...)HTML" 是 C11 原始字符串字面量，内容不转义，
 *  适合嵌入 HTML/CSS/JS。条件是内容中不能出现 ")HTML" 这个序列。
 * ═══════════════════════════════════════════════════════════════════ */
static const char kHtmlPage[] = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>健康监测终端 — i.MX6ULL</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js">
</script>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#0d1117;color:#c9d1d9;font-family:'Segoe UI',sans-serif;
       display:flex;flex-direction:column;align-items:center;padding:20px;min-height:100vh}
  h1{font-size:1.4em;margin-bottom:4px;color:#58a6ff}
  .sub{font-size:.75em;color:#8b949e;margin-bottom:16px}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;width:100%;max-width:1200px}
  .card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px}
  .card h2{font-size:.95em;color:#f0f6fc;margin-bottom:8px}
  canvas{width:100%!important;height:280px!important}
  .stats{display:flex;gap:24px;justify-content:center;margin-top:12px;flex-wrap:wrap}
  .stat{text-align:center;background:#161b22;border:1px solid #30363d;border-radius:8px;
         padding:12px 20px;min-width:100px}
  .stat .val{font-size:2em;font-weight:700;color:#58a6ff}
  .stat .lbl{font-size:.75em;color:#8b949e}
  #status{font-size:.75em;color:#3fb950;margin-top:12px}
  #status.err{color:#f85149}
</style>
</head>
<body>
<h1>健康监测终端</h1>
<div class="sub">i.MX6ULL — MPU6050 + MAX30102 | 实时刷新 2Hz</div>
<div class="stats">
  <div class="stat"><div class="val" id="hr">--</div><div class="lbl">心率 bpm</div></div>
  <div class="stat"><div class="val" id="spo2">--</div><div class="lbl">血氧 %</div></div>
  <div class="stat"><div class="val" id="temp">--</div><div class="lbl">温度 °C</div></div>
</div>
<div class="grid">
  <div class="card">
    <h2>MPU6050 加速度 (g)</h2>
    <canvas id="chart_accel"></canvas>
  </div>
  <div class="card">
    <h2>MPU6050 角速度 (°/s)</h2>
    <canvas id="chart_gyro"></canvas>
  </div>
</div>
<div id="status">● 已连接</div>

<script>
const MAX_PTS = 50;

function makeChart(id, title, datasets) {
  const ctx = document.getElementById(id).getContext('2d');
  return new Chart(ctx, {
    type:'line',
    data:{labels:Array(MAX_PTS).fill(''),datasets:datasets},
    options:{
      responsive:true,maintainAspectRatio:false,
      animation:{duration:100},
      scales:{
        x:{display:false},
        y:{grid:{color:'#21262d'},ticks:{color:'#8b949e',font:{size:10}}}
      },
      plugins:{legend:{labels:{color:'#c9d1d9',font:{size:10},boxWidth:12}}}
    }
  });
}

const colors = ['#58a6ff','#3fb950','#f0883e','#d2a8ff','#ff7b72','#79c0ff'];
const accel = makeChart('chart_accel', '加速度', ['ax','ay','az'].map((k,i)=>({
  label:k,data:Array(MAX_PTS).fill(null),borderColor:colors[i],borderWidth:1.5,
  pointRadius:0,tension:0.1
})));
const gyro = makeChart('chart_gyro', '角速度', ['gx','gy','gz'].map((k,i)=>({
  label:k,data:Array(MAX_PTS).fill(null),borderColor:colors[i+3],borderWidth:1.5,
  pointRadius:0,tension:0.1
})));

function push(chart, vals) {
  for(let i=0;i<vals.length;i++) {
    const ds = chart.data.datasets[i].data;
    ds.push(vals[i]); if(ds.length > MAX_PTS) ds.shift();
  }
  chart.update();
}

async function poll() {
  try {
    const r = await fetch('/api/sensors');
    if(!r.ok) throw new Error(r.status);
    const d = await r.json();
    document.getElementById('hr').textContent = d.ppg.hr || '--';
    document.getElementById('spo2').textContent = d.ppg.spo2 || '--';
    document.getElementById('temp').textContent = (d.mpu.temp||0).toFixed(1);
    push(accel, [d.mpu.ax, d.mpu.ay, d.mpu.az]);
    push(gyro, [d.mpu.gx, d.mpu.gy, d.mpu.gz]);
    document.getElementById('status').textContent='● 已连接';
    document.getElementById('status').className='';
  } catch(e) {
    document.getElementById('status').textContent='● 等待数据...';
    document.getElementById('status').className='err';
  }
}
setInterval(poll, 500);
poll();
</script>
</body>
</html>
)HTML";


/* ═══════════════════════════════════════════════════════════════════
 *  HTTP 请求处理回调
 *
 *  libmicrohttpd 的核心机制：每个 HTTP 请求到达时，MHD 内部线程调用
 *  此函数。函数根据 URL 和 method 决定返回什么内容。
 *
 *  GET /              → 返回嵌入式 HTML 仪表盘页面
 *  GET /api/sensors   → 返回最新的传感器 JSON 数据
 *  其他               → 404
 *
 *  upload_data / upload_data_size：
 *    对于 GET 请求，MHD 调用此函数时 *upload_data_size == 0，
 *    可以直接返回响应。对于 POST 请求，MHD 会多次调用，
 *    每次传入一段请求体数据，直到消费完毕。
 * ═══════════════════════════════════════════════════════════════════ */
static mhd_result_t http_handler(void *cls,
                        struct MHD_Connection *conn,
                        const char *url,
                        const char *method,
                        const char *version,
                        const char *upload_data,
                        size_t *upload_data_size,
                        void **con_cls)
{
    (void)cls;
    (void)version;
    (void)upload_data;   /* GET 路径不读请求体；有请求体时在下面 size 分支消费 */

    /*
     * 首次回调时记录新连接。
     *
     * libmicrohttpd 对每个 TCP 连接，第一次调用 handler 时 *con_cls == NULL，
     * 之后此连接的后续请求会保留我们设的值。这里用哨兵 (void*)1 标记已记录，
     * 避免 HTTP keep-alive 复用连接时重复打印连接日志。
     */
    if (*con_cls == NULL) {
        *con_cls = (void *)1;

        const union MHD_ConnectionInfo *info;
        info = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
        if (info != NULL) {
            const struct sockaddr_in *addr =
                (const struct sockaddr_in *)info->client_addr;
            tcp_log("[HTTP] 浏览器连接 %s:%d",
                    inet_ntoa(addr->sin_addr),
                    ntohs(addr->sin_port));
        }
    }

    /* POST 请求体未消费完时 MHD 会多次回调，这里处理完立即返回 */
    if (*upload_data_size > 0) {
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* ── 只处理 GET 请求 ── */
    if (strcmp(method, "GET") != 0) {
        return MHD_NO;  /* 405 Method Not Allowed */
    }

    /* ── GET / → 返回 HTML 页面 ── */
    if (strcmp(url, "/") == 0) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(kHtmlPage),
            (void *)kHtmlPage,
            MHD_RESPMEM_PERSISTENT  /* 页面是静态常量，MHD 不需要拷贝 */
        );
        if (resp == NULL) return MHD_NO;

        MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
        int ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/sensors → 返回传感器 JSON ── */
    if (strcmp(url, "/api/sensors") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t timestamp_ms = (uint64_t)ts.tv_sec * 1000
                               + ts.tv_nsec / 1000000;

        pthread_mutex_lock(&g_mpu_lock);
        mpu6050_data_t mpu = g_latest_mpu;
        pthread_mutex_unlock(&g_mpu_lock);

        pthread_mutex_lock(&g_ppg_lock);
        PPG_Result_t ppg = g_latest_ppg;
        pthread_mutex_unlock(&g_ppg_lock);

        char json[512];
        int len = snprintf(json, sizeof(json),
            "{\"t\":%llu,"
            "\"mpu\":{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
                      "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp\":%.1f},"
            "\"ppg\":{\"hr\":%d,\"spo2\":%d,\"valid\":%d}}",
            (unsigned long long)timestamp_ms,
            mpu.accel_x_g, mpu.accel_y_g, mpu.accel_z_g,
            mpu.gyro_x_dps, mpu.gyro_y_dps, mpu.gyro_z_dps, mpu.temp_c,
            ppg.heart_rate, ppg.spo2, ppg.data_valid
        );

        struct MHD_Response *resp = MHD_create_response_from_buffer(
            (size_t)len, json, MHD_RESPMEM_MUST_COPY  /* json 在栈上，必须拷贝 */
        );
        if (resp == NULL) return MHD_NO;

        MHD_add_response_header(resp, "Content-Type", "application/json; charset=utf-8");
        MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── 404 ── */
    {
        const char *body = "404 Not Found";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
        if (resp == NULL) return MHD_NO;
        int ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, resp);
        MHD_destroy_response(resp);
        return ret;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 *  HTTP 服务器线程
 *
 *  使用 libmicrohttpd 的 MHD_USE_INTERNAL_POLLING_THREAD 模式：
 *  MHD 在内部线程中执行 epoll 事件循环，本线程只需要存活并定时
 *  检查 g_running，退出时调用 MHD_stop_daemon 清理。
 *
 *  libmicrohttpd 自动处理：
 *    - TCP 连接管理（accept / close）
 *    - HTTP 协议解析（method / URL / headers）
 *    - 响应序列化和发送
 *    我们只需要在回调 http_handler 里根据 URL 返回内容。
 * ═══════════════════════════════════════════════════════════════════ */
void *thread_http_server(void *arg)
{
    (void)arg;

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD,
        HTTP_PORT,
        NULL, NULL,           /* accept policy — 允许所有连接 */
        &http_handler, NULL,  /* 回调 + 用户数据 */
        MHD_OPTION_END
    );

    if (daemon == NULL) {
        tcp_log("[HTTP] 启动失败！端口 %d 可能被占用", HTTP_PORT);
        return NULL;
    }

    tcp_log("[HTTP] 服务端已启动，端口: %d", HTTP_PORT);
    tcp_log("[HTTP] 浏览器访问 http://板子IP:%d", HTTP_PORT);

    /*
     * HTTP 守护进程在 MHD 内部线程中运行，本线程只负责存活检测。
     * 每 500ms 检查一次 g_running，收到退出信号后关闭 daemon。
     */
    while (g_running) {
        wd_beat(WD_CH_HTTP);
        usleep(500000);  /* 500ms */
    }

    MHD_stop_daemon(daemon);
    tcp_log("[HTTP] 服务端已停止");
    return NULL;
}
