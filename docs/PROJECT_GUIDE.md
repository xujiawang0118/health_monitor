# 健康与环境监测终端（health_monitor）

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

## 功能特性

| 功能 | 说明 |
|------|------|
| 传感器采集 | MPU6050（20Hz 轮询）、MAX30102（100Hz FIFO，中断 + 轮询兜底） |
| 终端显示 | 统一渲染线程，原始值 + 物理量 + 服务端日志 |
| TCP 服务端 | epoll 边缘触发多客户端，500ms 广播传感器 JSON |
| Python 客户端 | 接收 JSON → 终端展示 → 落盘 JSONL → 离线回放 |
| HTTP 仪表盘 | libmicrohttpd + Chart.js 实时波形（浏览器访问） |
| MQTT 上云 | libmosquitto，本地 broker / 华为云 IoTDA 双模式，TLS + HMAC-SHA256 |

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

### 线程模型（6 线程）

```
┌────────────────────────────────────────────────────────────────┐
│                          main.c                                 │
│     I2C 初始化 → 传感器初始化 → 创建 6 线程 → join → 清理        │
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
└──────────────┘└──────────┘└─────────┘└────────────────────────┘
```

| 线程 | 入口函数 | 技术要点 | 节奏 |
|------|---------|---------|------|
| MAX30102 采集 | `thread_max30102` | GPIO 中断 + poll 超时兜底 + FIFO 读取 + PPG 算法 | poll 50ms 超时 |
| MPU6050 采集 | `thread_mpu6050` | I2C 读原始值 + 物理量转换 | 50ms（20Hz） |
| 终端显示 | `thread_display` | 统一渲染 + 环形日志缓冲 | 500ms（2Hz） |
| TCP 服务端 | `thread_tcp_server` | epoll 边缘触发 + 非阻塞 accept/recv | epoll_wait 500ms 超时 |
| HTTP 服务端 | `thread_http_server` | libmicrohttpd 内部轮询线程 | — |
| MQTT 客户端 | `thread_mqtt` | libmosquitto + TLS + 华为云鉴权 | publish 500ms |

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
├── src/            # C 源码（9 个 .c）
│   ├── main.c          # 主入口：初始化 + 线程编排
│   ├── max30102.c      # MAX30102 驱动 + PPG 算法
│   ├── mpu6050.c       # MPU6050 驱动 + 物理量转换
│   ├── i2c_utils.c     # I2C 读写封装（互斥锁 + 重试）
│   ├── tcp_server.c    # TCP 工具函数（非阻塞/客户端管理）
│   ├── http_server.c   # libmicrohttpd + 内嵌 HTML
│   ├── mqtt_client.c   # libmosquitto 客户端（双模式）
│   ├── display.c       # 终端渲染
│   └── globals.c       # 全局变量 + 信号处理 + 日志缓冲
├── include/        # 头文件（9 个 .h）
│   ├── config.h        # 全部可调参数集中地（含 MQTT 云配置）
│   └── ...
├── client/
│   ├── client.py       # TCP 客户端：接收 + 落盘 JSONL + 离线回放
│   └── data/           # 落盘数据（.gitignore 忽略）
├── docs/           # 本文档
├── scripts/        # I2C 检查脚本 + 调试工具
├── build/          # 编译产物
└── Makefile        # 本地编译 / 交叉编译 / 检查
```

## 编译与部署

### 依赖库

| 库 | 用途 | 开发包 |
|----|------|--------|
| pthread / libm | 多线程、数学 | 系统自带 |
| libgpiod | GPIO 中断 | libgpiod-dev |
| libmicrohttpd | HTTP 服务 | libmicrohttpd-dev |
| libmosquitto | MQTT 客户端 | libmosquitto-dev |
| libssl / libcrypto | TLS + HMAC-SHA256 | libssl-dev |

### 本地编译（PC 端测试）

```bash
make          # 使用系统 gcc，动态链接
```

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
```

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

云端模式需在 `config.h` 填入华为云控制台的设备三元组（ProductKey / DeviceName / DeviceSecret）
和设备接入地址。鉴权密码 = `HMAC-SHA256(key=UTC时间戳YYYYMMDDHH, data=DeviceSecret)`，每小时过期，
板端时间须与 NTP 同步（偏差 <1 小时）。

> **安全提醒**：DeviceSecret 属敏感凭证，不要提交到公开仓库。建议改为环境变量或单独的
> 未跟踪配置文件。

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

## 已知限制与改进方向

- **MAX30102 中断实际未启用**：INT 引脚是开漏输出，硬件未接上拉电阻，INT 始终为低，
  中断不可用，主循环靠 poll 超时（50ms）轮询 FIFO 兜底。补上拉电阻后可真正启用中断驱动。
- **心率检测用固定阈值**（`PPG_PEAK_THRESH=800`），非自适应，个体差异和运动噪声会误检。
  可改进为自适应阈值（峰谷差比例法）。
- **SpO2 是经验公式**（`-45.06R² + 30.354R + 94.845`），非医疗器械级校准。
- **MPU6050 用 `usleep` 轮询**，实际周期 = 50ms + 读取耗时，长期有累积漂移。要精确采样率
  应基于单调时钟计算下次唤醒时间。
- **JSON 拼装重复**：TCP / HTTP / MQTT 三处拼的 JSON 结构几乎相同，可抽成 `build_sensor_json()`。
- **密钥硬编码**在 `config.h`，应外置。
