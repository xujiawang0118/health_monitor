# 健康与环境监测终端（health_monitor）— 项目指南

> **状态（2026-09 结项）**：功能与稳定性均已完成并部署验证 —— systemd 自启动、
> 看门狗复位、长跑无泄漏、I2C 总线不再瘫痪、断网自动重连全部通过；
> 唯一遗留是 Ctrl+C 偶发失效，已由 systemd + 看门狗绕过，留待后续复现定位。
> 面向 GitHub 访客的概览见 [`../README.md`](../README.md)。

## 项目概述

基于野火 i.MX6ULL 开发板（NXP Cortex-A7），通过 I2C 总线采集 MPU6050（六轴运动 + 温度）
和 MAX30102（心率 + 血氧）数据，在终端实时显示，并同时经 **TCP / HTTP / MQTT 三路**向外分发。

这是一个**学习型项目**，核心目标是通过它串起嵌入式 Linux 应用开发的主要知识点：

- Linux 用户空间 I2C 编程（`/dev/i2c-N` + `ioctl` + `read/write`）
- pthread 多线程与互斥锁同步
- 传感器驱动逻辑与 PPG（光电容积脉搏波）信号处理
- 信号处理与多线程优雅退出
- I/O 多路复用（poll + epoll 边缘触发）
- 网络编程（TCP 多客户端、HTTP 服务、MQTT/TLS 上云）
- 交叉编译与静态链接部署

在此之上，项目近期推进到**长时无人值守运行**这一层，也就是从"能跑通"到"能一直跑"：

- 硬件看门狗兜底 + 复位原因统计（`src/watchdog.c`）
- I2C 总线仲裁丢失的驱动级自动恢复（设备树 pinctrl/gpios 补丁）
- 内核源码级排查（定位到 `i2c-imx.c` 的 `-EAGAIN` 唯一来源、`imx2_wdt.c` 的 ops 表）

稳定性这一层的完整复盘见 `docs/i2c-bus-recovery.md`。

## 功能特性

| 功能 | 说明 |
|------|------|
| 传感器采集 | MPU6050（20Hz 轮询）、MAX30102（100Hz FIFO，中断 + 轮询兜底） |
| 终端显示 | 统一渲染线程，原始值 + 物理量 + 服务端日志 |
| TCP 服务端 | epoll 边缘触发多客户端，500ms 广播传感器 JSON |
| Python 客户端 | 接收 JSON → 终端展示 → 落盘 JSONL → 离线回放 |
| HTTP 仪表盘 | libmicrohttpd + Chart.js 实时波形（浏览器访问） |
| MQTT 上云 | libmosquitto，本地 broker / 华为云 IoTDA 双模式，TLS + HMAC-SHA256 |
| 硬件看门狗兜底 | 5 路心跳监视，卡死则整板复位；复位原因统计 + 启动日志 |
| I2C 总线恢复 | 设备树补 pinctrl GPIO 态 + `scl/sda-gpios`，仲裁丢失后驱动自动敲 SCL 唤醒 |
| Mock 模式 | `make EXTRA_CFLAGS=-DSENSOR_MOCK`，无硬件也能跑全部线程与数据链路（产物在 `build_mock/`） |

## 硬件需求

| 器件 | 接口 | I2C 地址 | 说明 |
|------|------|----------|------|
| MPU6050 | I2C | 0x68（AD0=GND） | 3 轴加速度 + 3 轴陀螺仪 + 温度 |
| MAX30102 | I2C | 0x57 | 心率 + 血氧（红光 660nm / 红外 880nm） |
| MAX30102 INT | GPIO | gpiochip2 line 27 | FIFO 满中断引脚（见下方"已知限制"） |

接线：

```
MPU6050:  VCC→3.3V  GND→GND  SCL→I2C_SCL  SDA→I2C_SDA  (AD0→GND)
MAX30102: VCC→3.3V  GND→GND  SCL→I2C_SCL  SDA→I2C_SDA  (INT→GPIO)
```

## 软件架构

### 线程模型（7 线程）

```
┌────────────────────────────────────────────────────────────────┐
│                          main.c                                 │
│  I2C 初始化 → 传感器初始化 → wd_init() → 创建 7 线程 → join → 清理 │
└───────┬──────────────────────────────────────────────────────────┘
        │
   ┌────┴───────────────────────────────────────────────────────┐
   │                    共享数据区（互斥锁保护）                   │
   │   g_latest_mpu / g_latest_ppg / tcp_log 环形缓冲            │
   └────┬──────────┬──────────┬──────────┬──────────────────────┘
        │          │          │          │
┌───────▼─────┐┌───▼──────┐┌──▼──────┐┌──▼─────────────────────┐
│ max30102 线程││ mpu6050  ││ display ││  tcp / http / mqtt 线程 │
│ 中断+轮询FIFO││  轮询    ││ 统一渲染 ││  三路数据分发            │
│ PPG 算法     ││  20Hz    ││  2Hz    ││  6666/8080/broker        │
└──────┬──────┘└───┬──────┘└──┬──────┘└──┬─────────────────────┘
       │           │          │          │
       │  wd_beat()│          │          │   ← 心跳（每轮循环开头打一次）
       └───────────┴──────────┴──────────┘
                          │
              ┌───────────▼────────────┐
              │   thread_watchdog      │  2s 巡检：任一槽位 20s 无心跳
              │   巡检 + 喂狗 + 统计    │  → 停止喂狗 → 30s 后硬件复位整板
              └────────────────────────┘
```

| 线程 | 入口函数 | 技术要点 | 节奏 |
|------|---------|---------|------|
| MAX30102 采集 | `thread_max30102` | GPIO 中断 + poll 超时兜底 + FIFO 读取 + PPG 算法 | poll 50ms 超时 |
| MPU6050 采集 | `thread_mpu6050` | I2C 读原始值 + 物理量转换 + 连续异常软复位 | 50ms（20Hz） |
| 终端显示 | `thread_display` | 统一渲染 + 环形日志缓冲 | 500ms（2Hz） |
| TCP 服务端 | `thread_tcp_server` | epoll 边缘触发 + 非阻塞 accept/recv | epoll_wait 500ms 超时 |
| HTTP 服务端 | `thread_http_server` | libmicrohttpd 内部轮询线程 | — |
| MQTT 客户端 | `thread_mqtt` | libmosquitto + TLS + 华为云鉴权 | publish 500ms |
| 看门狗巡检 | `thread_watchdog` | 心跳停滞判定 + 喂狗 + 复位统计落盘 | 2000ms |

> MQTT 线程**故意不纳入**心跳监视：`mosquitto_connect()` 阻塞几十秒是正常重连行为，
> 算进健康集合会让一次网络抖动变成一次整板重启。详见 `src/watchdog.h` 的入选标准说明。

### 数据流

```
MPU6050 ──I2C(20Hz)──> thread_mpu6050 ──> g_latest_mpu ─┐
MAX30102 ─I2C(100Hz)─> thread_max30102 ─> g_latest_ppg ─┤
                                                        ├─> display（终端）
                                                        ├─> TCP :6666 ──> client.py（落盘/回放）
                                                        ├─> HTTP :8080 ─> 浏览器 Chart.js
                                                        └─> MQTT ──> broker / 华为云 IoTDA
```

各工作线程只写共享数据（加锁），消费端各自加锁读取。终端输出由 display 线程统一渲染，
其余线程通过 `tcp_log()` 写入环形缓冲，避免多线程 `printf` 争抢 stdout。

## 目录结构

```
health_monitor/
├── README.md       # 项目门面：亮点、架构图、快速开始、部署
├── src/            # C 源码（10 个 .c）
│   ├── main.c          # 主入口：初始化 + 线程编排 + 带超时 join
│   ├── watchdog.c      # 硬件看门狗 + 复位计数 + 启动日志（734 行）
│   ├── max30102.c      # MAX30102 驱动 + PPG 算法
│   ├── mpu6050.c       # MPU6050 驱动 + 物理量转换
│   ├── i2c_utils.c     # I2C 读写封装（互斥锁 + 重试 + errno 分类）
│   ├── tcp_server.c    # TCP 工具函数（非阻塞/客户端管理）
│   ├── http_server.c   # libmicrohttpd + 内嵌 HTML
│   ├── mqtt_client.c   # libmosquitto 客户端（双模式）
│   ├── display.c       # 终端渲染
│   └── globals.c       # 全局变量 + 信号处理 + 日志缓冲
├── include/        # 头文件（11 个 .h）
│   ├── config.h                # 全部可调参数集中地（含 MQTT 云配置、看门狗阈值）
│   ├── watchdog.h              # 看门狗接口 + 被监视槽位定义
│   ├── cloud_secret.h.example  # 云平台三元组模板
│   └── ...
├── client/
│   ├── client.py       # TCP 客户端：接收 + 落盘 JSONL + 离线回放
│   └── data/           # 落盘数据（.gitignore 忽略）
├── docs/
│   ├── PROJECT_GUIDE.md      # 本文档：架构、编译、部署、设计决策
│   └── i2c-bus-recovery.md   # i2c-0 总线瘫痪复盘（含内核源码定位 + 面试 Q&A）
├── scripts/        # I2C 检查脚本 + 长跑采样 + systemd unit
│   ├── health_monitor.service  # systemd unit（部署时拷到 /etc/systemd/system/）
│   ├── check_i2c.sh            # 板端 I2C 设备检查
│   └── soak_watch.sh           # 长跑采样：RSS / fd 数 / 线程数
├── build/          # 编译产物
└── Makefile        # 本地编译 / 交叉编译 / 检查
```

## 编译与部署

### 依赖库

| 库 | 用途 | 开发包 |
|----|------|--------|
| pthread / libm | 多线程、数学 | 系统自带 |
| libgpiod | GPIO 中断 | libgpiod-dev，**必须是 1.x**（2.x 改了 API，见下） |
| libmicrohttpd | HTTP 服务 | libmicrohttpd-dev |
| libmosquitto | MQTT 客户端 | libmosquitto-dev |
| libssl / libcrypto | TLS + HMAC-SHA256 | libssl-dev |

> **libgpiod 版本坑**：`max30102.c` 里用的是 libgpiod **1.x** 的 line API
> （`gpiod_chip_open` / `gpiod_chip_get_line` / `gpiod_line_request_both_edges_events`
> / `gpiod_line_event_get_fd` / `gpiod_line_event_read`）。2.x 把这套整体重做了 ——
> `gpiod_chip_open` 变成 `gpiod_chip_open_path`，line 变成不透明指针
> `gpiod_line_request`，事件读取也换了对象类型。**升到 2.x 直接编不过**。
>
> 板端 Debian 10 自带 1.x，所以交叉编译不受影响；Ubuntu 24.04 / Debian 13 之后
> 默认装的是 2.x，PC 端本地编译会失败。绕法：用 mock 模式（不走 GPIO），
> 或从源码装 libgpiod 1.6.x。要彻底解决得写一层 v1/v2 兼容 shim。

### 本地编译（PC 端测试）

```bash
make                                # 使用系统 gcc，动态链接
make EXTRA_CFLAGS=-DSENSOR_MOCK     # mock 模式（产物在 build_mock/，与 build/ 隔离）
make ASAN=1                         # AddressSanitizer（产物在 build_asan/）
```

三种模式输出到各自独立的目录。**这不是洁癖**：`EXTRA_CFLAGS` 变了但 `.c` 没改时，
make 认为 `.o` 还是新的、不会重编，会出现"加了 `-DSENSOR_MOCK` 却依然去开硬件"的
鬼故事。分目录之后切换模式无需手动 `clean`。

### 交叉编译（板端运行）

```bash
make cross    # arm-linux-gnueabihf-gcc，静态链接
```

静态链接原因：板端 Debian 的 glibc 较老（2.28），宿主交叉工具链 glibc 较新（2.40+），
动态链接会导致 `GLIBC_2.34 not found`。静态链接后二进制自包含（约 700KB）。

> 注意：交叉静态链接需要提前准备上述各库的 armhf `.a` 静态库文件。

### 板端运行

```bash
# 1. 检查 I2C 设备是否可见
sudo i2cdetect -y 0     # 应看到 0x57(MAX30102) 和 0x68(MPU6050)

# 2. （MQTT 本地模式）启动 broker
mosquitto -d

# 3. 运行程序（需 root 权限访问 I2C 和 GPIO）
sudo ./health_monitor

# 无硬件时（PC 端验证线程/数据链路）：
make EXTRA_CFLAGS=-DSENSOR_MOCK && ./build_mock/health_monitor
```

统计文件默认落在 `~/demo/`（`watchdog_stats.conf` + `watchdog_boots.log`），
可用 `HEALTH_MONITOR_STATE_DIR=/path` 覆盖。

### systemd 托管（24h 无人值守）

手动 `sudo ./health_monitor` 只适合调试。真正无人值守必须交给 systemd，
否则看门狗复位后没人把程序拉起来，**也就没人来记账** —— 见下方"记账时机反直觉"。

```bash
sudo mkdir -p /opt/health_monitor
sudo cp build/health_monitor /opt/health_monitor/

sudo cp scripts/health_monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now health_monitor

systemctl status health_monitor
journalctl -u health_monitor -f
```

unit 文件（`scripts/health_monitor.service`）里几个不显然但必要的设置：

| 设置 | 为什么 |
|------|--------|
| `Environment=HEALTH_MONITOR_STATE_DIR=...` | **必须显式设**。systemd 下没有 `SUDO_USER`、`$HOME` 是 `/root`，不设就会落到 `/root/...`，和手动 sudo 跑出来的统计**分家**。unit 里用 `StateDirectory=health_monitor` → `/var/lib/health_monitor`（系统服务的规范做法，不依赖家目录） |
| `Restart=always` + `StartLimitBurst=5` / `StartLimitIntervalSec=300` | 前者保证复位后自动拉起；后者防**启动循环**（I2C 一上来就卡 → 复位 → 又卡）。300 秒内超过 5 次就进入 failed 停止拉起，留一个可登录的 shell |
| `StandardOutput=null` | display 线程每秒刷一屏 ANSI 转义序列（整屏重绘），进 journal 只会把日志淹掉 |
| `StandardError=journal` | 有价值的告警都走 stderr（`[WATCHDOG]` / `[WARN]` / `[ERROR]`），保留 |
| `TimeoutStopSec=30` | 收到 SIGTERM 后协作式收尾最坏约 15 秒（3 + 2×6），留余量。走完收尾会调到 `wd_stop()` 写 `last_clean=1`，于是 `systemctl stop` 在账本上是**一次干净退出**；被 SIGKILL 则不是 |
| `User=root` | 要开 `/dev/i2c-0`、gpiochip、`/dev/watchdog`（默认 `0600 root:root`） |

## 数据出口

### 1. TCP（:6666）— Python 客户端

```bash
# 实时采集（自动落盘到 client/data/*.jsonl）
python3 client/client.py 172.20.10.2 6666

# 离线回放
python3 client/client.py --replay 文件.jsonl --speed 1.0
```

存储格式选 JSONL 而非 CSV：字段可嵌套、无类型丢失、追加写入不丢数据、pandas/jq 可直接消费。

### 2. HTTP（:8080）— Web 仪表盘

浏览器访问 `http://板子IP:8080`，页面内嵌 Chart.js，每 500ms 拉取 `/api/sensors` 渲染
MPU6050 六轴波形 + 心率/血氧数值。

### 3. MQTT — 本地 broker / 华为云 IoTDA

见下方配置说明。

## MQTT 配置（本地 / 云端双模式）

切换方式：`include/config.h` 中的 `MQTT_CLOUD_MODE` 宏。

- **本地模式**（默认注释掉宏）：连接 `localhost:1883`，板端自建 mosquitto broker，无加密无认证。
- **云端模式**（取消注释）：连接华为云 IoTDA，TLS 8883 端口 + HMAC-SHA256 鉴权。

云端模式的三元组（ProductKey / DeviceName / DeviceSecret）和设备接入地址
放在**未纳入版本控制**的 `include/cloud_secret.h` 中，仓库里只提交模板：

```bash
cp include/cloud_secret.h.example include/cloud_secret.h
# 填入华为云控制台 → 设备详情 → 设备信息 → 接入信息 中的真实值
```

`config.h` 在 `MQTT_CLOUD_MODE` 打开时 `#include "cloud_secret.h"`，
而该文件已在 `.gitignore` 中排除。鉴权密码 =
`HMAC-SHA256(key=UTC时间戳YYYYMMDDHH, data=DeviceSecret)`，每小时过期，
板端时间须与 NTP 同步（偏差 <1 小时）。

> **安全提醒**：DeviceSecret 属敏感凭证。改动 `.gitignore` 或重命名该文件前，
> 先确认它不会被 `git add` 进去 —— 一旦进了历史，删文件也删不掉历史。

## 关键设计决策

| 决策 | 原因 |
|------|------|
| I2C 用 `ioctl(I2C_SLAVE)` + `write/read` 而非 `ioctl(I2C_RDWR)` | 兼容性最好，i.MX6ULL 4.19 内核老，方式 A 更稳；20Hz 采样性能足够 |
| I2C 地址缓存 + 跳过重复 ioctl | 4.19 内核反复 I2C_SLAVE ioctl 会触发状态重置导致 EAGAIN |
| MAX30102 中断 + poll 超时兜底 | 见下方"已知限制"，硬件中断不可用时轮询兜底 |
| epoll 边缘触发 + 非阻塞 | 循环 accept/recv 到 EAGAIN，避免漏事件 |
| `sigaction` 不加 `SA_RESTART` | 多线程下 SIGINT 投递给任意线程，若 SA_RESTART 导致系统调用自动重试，线程卡死无法退出 |
| TCP 服务端 epoll_wait 500ms 超时 | 保证没收到信号的 TCP 线程也能定期检查 `g_running` 退出 |
| 静态链接 | 绕过板端 glibc 版本不匹配 |
| 数据落盘放 PC 端（client.py） | 板端存储有限，采集与存储分离，PC 端 JSONL 更灵活 |
| `tcp_log` 环形缓冲 + display 单线程渲染 | 避免多线程 printf 混叠 |
| 看门狗**故意不监视 MQTT 线程** | `mosquitto_connect()` 阻塞几十秒是正常重连，算进去等于让网络抖动触发整板重启 |
| 心跳打在循环**开头** | "我活着、马上要干活"和"我刚干完活"含义统一，心跳间隔 = 一轮循环真实耗时 |
| 看门狗超时用 `#error` 编译期钉死 ≤128s | 超限后内核会替卡死的进程喂狗，保护静默失效 —— 必须让编译失败而不是运行时才发现 |
| 主线程用**带超时的 join** 而非无限 `pthread_join` | I2C 卡死时仍能保证进程可退出，不会像之前那样整个卡住 |
| 退出路径**故意不调 `wd_stop()`**（异常分支） | 保留 fd 引用，让内核在进程彻底消失后接管 —— 避免退出竞态期间无人喂狗 |

## 长时间运行保障（稳定性设计）

项目从"能跑通"推进到"能无人值守长跑"，这一层的核心是三件事：

### 1. 硬件看门狗兜底（`src/watchdog.c`）

分工设计成：**进程活着 → 进程喂；进程干净退出 → 内核喂；进程活着但卡死 → 没人喂 → 复位**。
第三行正是要的效果，且不是靠显式关设备实现的，而是"没人持有 fd"这个物理事实自然带来的。

三条反直觉事实（本板内核 4.19.35，均在 `imx2_wdt.c` 里核实过）：

- **看门狗硬件上停不了**：`imx2_wdt.c:239` 的 ops 表里没有 `.stop`，"magic close 关掉"不成立。
  但正常退出**不会**把板子喂重启 —— `watchdog_dev.c` 在驱动无 `.stop` 时置 `WDOG_HW_RUNNING`，
  随后内核接管喂狗（`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED` 的语义）。
- **超时必须 ≤ 128 秒**：`max_hw_heartbeat_ms = 128000`。一旦超过，`watchdog_need_worker()`
  的第一个条件成立 → **卡死时内核照样替你喂**，保护完全失效。`watchdog.c` 用 `#error`
  在编译期钉死这条。
- **停滞判定按真实流逝时间算**（`CLOCK_MONOTONIC` 毫秒），不按巡检轮数。按轮数算错过一次：
  睡眠参数非法导致自旋，微秒级就把正常线程全判成故障。

阈值链：心跳停滞 20s 判定 → 停止喂狗 → 再等 30s → 硬件复位。20s 的由来是 I2C 总线
被拉死时单次读最坏 10s 以上，MAX30102 还要连读 `fifo_count` 个样本。

### 2. I2C 总线自动恢复（设备树补丁）

详见 `docs/i2c-bus-recovery.md`。简要：i.MX6ULL 的 `i2c-imx` 驱动**内置**了总线恢复
（切 GPIO 敲 9 拍 SCL 把从机敲醒），但设备树没给它接上 `pinctrl "gpio"` 状态和
`scl-gpios`/`sda-gpios`，导致一次仲裁丢失（`errno=11`，即 `I2SR_IAL`）就永久报废。

补上三个必要条件后，驱动侧自动恢复。**注意**：恢复成功的路径全是 `dev_dbg` 级日志，
`dev_err` 只在恢复失败时打 —— 所以"没日志"不等于"没生效"。

### 3. 启动计数与防误判

统计文件路径运行时解析：`HEALTH_MONITOR_STATE_DIR` 环境变量 → sudo 下用 `SUDO_USER`
查 passwd 要家目录 → `$HOME` → `./`。**sudo 会把 `HOME` 重置成 `/root`**，必须靠
`SUDO_USER` 还原原始用户，否则统计静默分裂成两份。

同时记两个计数器：`unclean_boots`（靠应用自写标记，不依赖硬件语义，永远可靠）和
`wd_resets`（靠 `WDIOC_GETBOOTSTATUS` 的 `WDIOF_CARDRESET` 位，已实测可用）。
前者是主证据，后者是旁证。

> **记账时机反直觉**："上次怎么退出的"这条判定是在**程序下一次启动时**才写进日志的 ——
> 复位期间没有代码在跑。所以程序不自启动的话，复位后没人来记账，`cat` 看到的还是旧快照。
> 24h 无人值守必须配 systemd `Restart=always`。

### 结项验证结果

补齐上面三层之后，本阶段（2026-09）的收尾验证：

| 验证项 | 结果 |
|--------|------|
| systemd 自启动 + 崩溃自动拉起 | 通过，复位后自动接管并补记上一次退出原因 |
| 硬件看门狗复位功能 | 通过。冻结整个进程 30 秒后板子复位，下次启动 `WDIOC_GETBOOTSTATUS` 正确带回 `WDIOF_CARDRESET`，日志打出"上次由看门狗超时复位"，`wd_resets` 0→1 |
| 长期运行稳定性（RSS / fd / 线程数） | 通过，长时间运行未出现异常增长（用 `scripts/soak_watch.sh` 采样） |
| I2C 总线不再瘫痪 | 通过，长时间运行未再复现仲裁丢失 |
| 断网重连（MQTT） | 通过，`mosquitto_connect()` 阻塞数十秒后正常重连，未触发看门狗（验证了"MQTT 排除在心跳集合之外"这个决策是对的） |
| Ctrl+C 优雅退出 | **未闭环**，见下方已知限制 |

## 已知限制与改进方向

- **Ctrl+C 偶发失效（未闭环，已暂避）**：个别情况下 `SIGINT` 无法让进程退出，
  且已排除"总线瘫痪是唯一原因"（有一次 1 小时运行无瘫痪但 Ctrl+C 依然不灵）。
  **从未真正抓到卡住时的内核栈**，这是最该补的一步：

  ```bash
  PID=$(pidof health_monitor)               # 别用 pgrep -f，会多匹配到 sudo 父进程
  ps -L -o lwp,stat,wchan:32,comm -p $PID   # 看哪个线程是 D、卡在哪个内核函数
  for t in /proc/$PID/task/*; do sudo cat $t/stack; done
  echo w | sudo tee /proc/sysrq-trigger; sudo dmesg | tail -150
  ```

  头号嫌疑是 `i2c_transfer` 里拿 `adap->bus_lock` 的 `__mutex_lock`（其等待状态是
  `TASK_UNINTERRUPTIBLE`，比总线本身更难恢复）；次要嫌疑是本版 4.19 的
  `i2c_imx_xfer()` 每次传输都做一对 `pm_runtime_enable/disable`，`pm_runtime_disable()`
  内部有 barrier。**当前由 systemd 托管 + 看门狗兜底绕过**：即使卡住也能在约 50 秒内
  整板复位后自动拉起，不再需要人工拔电。留待后续复现时再抓栈定位。

- **启动循环的防线在 systemd 层，应用层没有闸**：目前靠 unit 里
  `StartLimitBurst=5` / `StartLimitIntervalSec=300` 兜底。更彻底的做法是应用层加闸：
  连续 N 次非正常退出就**拒绝接管看门狗**（`wd_init()` 返回失败但程序继续跑），
  把 shell 留给人工。两层的区别是——systemd 停了服务，应用层停了才有日志可查。
- **MPU6050 连续异常后软复位尚无退避**：当前"连续 3 次异常 → 软复位"每 0.42 秒往死总线上
  再捅一刀。应改为指数退避 + 总线故障标志。**注意**：退避若导致线程停滞超过 20s 心跳阈值
  会把板子喂重启，两处必须一起设计。
- **MAX30102 中断实际未启用**：INT 引脚是开漏输出，硬件未接上拉电阻，INT 始终为低，
  中断不可用，主循环靠 poll 超时（50ms）轮询 FIFO 兜底。补上拉电阻后可真正启用中断驱动。
- **心率检测用固定阈值**（`PPG_PEAK_THRESH=800`），非自适应，个体差异和运动噪声会误检。
  可改进为自适应阈值（峰谷差比例法）。
- **SpO2 是经验公式**（`-45.06R² + 30.354R + 94.845`），非医疗器械级校准。
- **MPU6050 用 `usleep` 轮询**，实际周期 = 50ms + 读取耗时，长期有累积漂移。要精确采样率
  应改用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` + 绝对时刻。
- **JSON 拼装重复**：TCP / HTTP / MQTT 三处拼的 JSON 结构几乎相同，可抽成 `build_sensor_json()`。
- ~~**密钥硬编码**在 `config.h`~~ **已解决**：三元组移入未纳入版本控制的 `include/cloud_secret.h`，
  仓库只提交 `cloud_secret.h.example` 模板。更彻底的做法是走环境变量或 KMS。
