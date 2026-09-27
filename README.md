# health_monitor — 基于 i.MX6ULL 的健康与环境监测终端

在野火 EBF6ull Pro（NXP i.MX6ULL，Cortex-A7）上运行的嵌入式 Linux 应用：
通过 I2C 采集 **MPU6050**（六轴 + 温度）与 **MAX30102**（心率 + 血氧）数据，
在终端实时渲染，并同时经 **TCP / HTTP / MQTT 三路**向外分发。
面向**长时无人值守**场景，带硬件看门狗兜底与 I2C 总线自动恢复。

```
MPU6050 ──I2C(20Hz)──┐
                     ├─► 共享数据区 ──┬─► 终端显示（2Hz 统一渲染）
MAX30102 ─I2C(100Hz)─┘   （互斥锁）   ├─► TCP  :6666 ──► client.py（JSONL 落盘 / 回放）
                                      ├─► HTTP :8080 ──► 浏览器 Chart.js 仪表盘
                                      └─► MQTT       ──► 本地 broker / 华为云 IoTDA（TLS）
```

---

## 目录

- [亮点](#亮点)
- [硬件平台](#硬件平台)
- [软件架构](#软件架构)
- [快速开始](#快速开始)
- [部署到开发板](#部署到开发板)
- [数据出口](#数据出口)
- [长时运行保障](#长时运行保障)
- [目录结构](#目录结构)
- [文档索引](#文档索引)
- [已知限制](#已知限制)

---

## 亮点

这个项目的重点不在"读两个传感器"，而在**能从"跑通"推进到"一直跑"**。
下面几条都是踩过坑之后落到代码/设备树里的：

| 亮点 | 说明 |
|------|------|
| **内核源码级故障定位** | 现场表现为 `errno=11`，回源码定位到 `i2c-imx.c` 全文件**唯一**的 `-EAGAIN` 返回点（`I2SR_IAL` 仲裁丢失位），区分开与之易混的 `-ETIMEDOUT`。见 [i2c 复盘文档](docs/i2c-bus-recovery.md) |
| **设备树补丁修复总线瘫痪** | i2c-imx **自带**总线恢复（切 GPIO 敲 9 拍 SCL），但设备树缺 `pinctrl "gpio"` 态与 `scl/sda-gpios`，导致一次仲裁丢失永久报废。补齐三个必要条件后恢复通路接通 |
| **硬件看门狗兜底** | 5 路线程心跳监视，停滞 20s 停止喂狗 → 30s 后整板复位。复位原因跨重启记账（`unclean_boots` + `wd_resets`） |
| **看门狗超时编译期钉死** | `imx2_wdt` 的 `max_hw_heartbeat_ms = 128000`，超限后内核会替卡死进程喂狗、保护静默失效。用 `#error` 让它在编译期就失败 |
| **systemd 托管** | `Restart=always` + 启动计数，复位后自动接管并补记上一次的退出原因 |
| **可无硬件运行** | `make EXTRA_CFLAGS=-DSENSOR_MOCK` 用假数据跑通全部线程与数据链路，PC 端即可验证 |

---

## 硬件平台

| 器件 | 接口 | 地址 | 说明 |
|------|------|------|------|
| 主控 | — | — | 野火 EBF6ull Pro，NXP i.MX6ULL Cortex-A7 |
| MPU6050 | I2C-0 | `0x68`（AD0=GND） | 3 轴加速度 + 3 轴陀螺仪 + 温度 |
| MAX30102 | I2C-0 | `0x57` | 心率 + 血氧（红光 660nm / 红外 880nm） |
| MAX30102 INT | GPIO | gpiochip2 line 27 | FIFO 中断引脚（见[已知限制](#已知限制)） |

```
MPU6050:  VCC→3.3V  GND→GND  SCL→I2C_SCL  SDA→I2C_SDA  (AD0→GND)
MAX30102: VCC→3.3V  GND→GND  SCL→I2C_SCL  SDA→I2C_SDA  (INT→GPIO)
```

> **命名陷阱**：设备树里这条总线叫 `&i2c1`（`0x021a0000`），
> 但 Linux 枚举成 **`/dev/i2c-0`**，错位一个。排查时看到 `i2c1` 别以为"那是另一条总线"。
> 该总线上还有内核驱动的 **GT1151 电容触摸屏（`0x14`）**。

---

## 软件架构

### 线程模型（7 线程 = 6 功能 + 1 巡检）

```
┌────────────────────────────────────────────────────────────────┐
│                          main.c                                 │
│  I2C 初始化 → 传感器初始化 → wd_init() → 创建 7 线程 → join → 清理 │
└────────────────────────────────────────────────────────────────┘
        │
   ┌────┴───────────────────────────────────────────────────────┐
   │                 共享数据区（互斥锁保护）                      │
   │      g_latest_mpu / g_latest_ppg / tcp_log 环形缓冲          │
   └────┬──────────┬──────────┬──────────┬──────────────────────┘
        │          │          │          │
┌───────▼─────┐┌───▼──────┐┌──▼──────┐┌──▼─────────────────────┐
│ max30102    ││ mpu6050  ││ display ││ tcp / http / mqtt       │
│ 中断+轮询FIFO││  轮询    ││统一渲染  ││ 三路数据分发             │
│ PPG 算法     ││  20Hz    ││  2Hz    ││ 6666 / 8080 / broker    │
└──────┬──────┘└───┬──────┘└──┬──────┘└──┬─────────────────────┘
       └───────────┴──────────┴──────────┘
                          │ wd_beat() 每轮循环开头打一次心跳
              ┌───────────▼────────────┐
              │   thread_watchdog      │  2s 巡检：任一槽位 20s 无心跳
              │   巡检 + 喂狗 + 统计    │  → 停止喂狗 → 30s 后硬件复位整板
              └────────────────────────┘
```

| 线程 | 技术要点 | 节奏 |
|------|---------|------|
| MAX30102 采集 | GPIO 中断 + `poll` 超时兜底 + FIFO 读取 + PPG 算法 | poll 50ms |
| MPU6050 采集 | I2C 读原始值 + 物理量转换 + 连续异常软复位 | 50ms（20Hz） |
| 终端显示 | 统一渲染 + 环形日志缓冲（避免多线程 `printf` 混叠） | 500ms（2Hz） |
| TCP 服务端 | `epoll` 边缘触发 + 非阻塞 `accept/recv` | `epoll_wait` 500ms |
| HTTP 服务端 | libmicrohttpd + 内嵌 Chart.js 页面 | — |
| MQTT 客户端 | libmosquitto + TLS + 华为云 HMAC-SHA256 鉴权 | 500ms / 30s |
| 看门狗巡检 | 心跳停滞判定 + 喂狗 + 复位统计落盘 | 2000ms |

> **MQTT 线程故意不纳入心跳监视**：`mosquitto_connect()` 阻塞几十秒是正常重连行为，
> 算进健康集合会让一次网络抖动变成一次整板重启。

### 关键设计决策

| 决策 | 原因 |
|------|------|
| I2C 用 `ioctl(I2C_SLAVE)` + `read/write` 而非 `I2C_RDWR` | 兼容 i.MX6ULL 上的老内核（4.19），20Hz 采样性能足够 |
| I2C 地址缓存 + 跳过重复 `I2C_SLAVE` ioctl | 4.19 内核反复下发该 ioctl 会触发控制器状态重置导致 `EAGAIN` |
| `sigaction` **不加** `SA_RESTART` | 多线程下 SIGINT 投递给任意线程，`SA_RESTART` 会让系统调用自动重试，线程卡死无法退出 |
| 所有线程用 `pthread_timedjoin_np` 带超时回收 | 有线程卡在内核态时，裸 `pthread_join` 会把主线程一起拖住，进程永远退不出去 |
| 退出阶段有线程未回收则**跳过清理直接 `_exit()`** | 卡住的线程可能正持有 `g_i2c_fd`，此时 `close()` 会因 fd 号复用而误伤 |
| TCP `epoll_wait` 带 500ms 超时 | 保证没收到信号的 TCP 线程也能定期检查 `g_running` 退出 |
| 静态链接交叉编译 | 绕过板端 Debian 10（glibc 2.28）与工具链（2.40+）的版本不匹配 |
| 数据落盘放 PC 端 | 板端存储有限，采集与存储分离；JSONL 字段可嵌套、追加写入不丢数据、`jq`/pandas 可直接消费 |

---

## 快速开始

### 依赖

```bash
# Debian / Ubuntu
sudo apt install build-essential pkg-config \
                 libgpiod-dev libmicrohttpd-dev libmosquitto-dev libssl-dev
```

| 库 | 用途 |
|----|------|
| pthread / libm | 多线程、数学 |
| libgpiod | GPIO 中断 |
| libmicrohttpd | HTTP 服务 |
| libmosquitto | MQTT 客户端 |
| libssl / libcrypto | TLS + HMAC-SHA256 鉴权 |

> ⚠ **libgpiod 需要 v1.x**。本项目用的 `gpiod_chip_open` / `gpiod_line_request_*`
> 是 libgpiod **1.x** 的 API，2.x 把它们整体改名成了 `gpiod_chip_open_path` /
> `gpiod_line_request_*`（对象变成不透明指针），直接编不过。
> 板端 Debian 10 自带的就是 1.x，所以交叉编译没问题；
> 但 Ubuntu 24.04 / Debian 13 之后默认装的是 2.x，**PC 端本地编译会失败**。
> 用 mock 模式（不需要 GPIO）或从源码装 libgpiod 1.6.x 可绕过。

### 编译

```bash
make                                # 本机 gcc，动态链接（PC 端测试）
make EXTRA_CFLAGS=-DSENSOR_MOCK     # mock 模式，产物在 build_mock/
make ASAN=1                         # 本机 gcc + AddressSanitizer（产物在 build_asan/）
make cross                          # arm-linux-gnueabihf-gcc，静态链接（板端运行）
make clean
```

三种模式输出到各自独立的目录，互不污染 —— 否则改了宏但 `.o` 被 make 认为是新的不再重编，
会出现"加了 `-DSENSOR_MOCK` 却还是去开硬件"的鬼故事。

### 无硬件运行（Mock 模式）

```bash
make EXTRA_CFLAGS=-DSENSOR_MOCK && ./build_mock/health_monitor
```

用假数据跑通全部 7 个线程与 TCP/HTTP/MQTT 数据链路，无需任何传感器。
（PC 端验证线程调度、网络分发、看门狗记账逻辑都靠这个模式。）

### 板端运行

```bash
sudo i2cdetect -y 0        # 应看到 0x14(触摸屏) / 0x57(MAX30102) / 0x68(MPU6050)
sudo ./health_monitor      # 需要 root：访问 /dev/i2c-0、GPIO、/dev/watchdog
```

---

## 部署到开发板

### 1. 交叉编译并传输

交叉静态链接需要提前准备各依赖库的 armhf `.a` 文件。

```bash
make cross
# 板子跑 dropbear，scp 不可用 —— 用 PC 端起 http.server，板端 wget 拉取
python3 -m http.server 8000
# 板端：wget http://<PC_IP>:8000/build/health_monitor
```

### 2. MQTT 配置

`include/config.h` 的 `MQTT_CLOUD_MODE` 宏切换本地 / 云端双模式：

- **本地模式**：连 `localhost:1883`，板端自建 `mosquitto -d`，无加密无认证。
- **云端模式**：连华为云 IoTDA，TLS 8883 + HMAC-SHA256 鉴权。

云端模式的三元组（ProductKey / DeviceName / DeviceSecret）放在**未纳入版本控制**的
`include/cloud_secret.h` 中，仓库里只有模板：

```bash
cp include/cloud_secret.h.example include/cloud_secret.h
# 然后填入控制台获取的真实值
```

鉴权密码 = `HMAC-SHA256(key=UTC时间戳YYYYMMDDHH, data=DeviceSecret)`，每小时过期，
板端时间需与 NTP 同步（偏差 <1 小时）。

### 3. systemd 托管（推荐）

24h 无人值守必须配自启动 —— 看门狗的复位统计是在**程序下次启动时**才补记的，
不自启动就没人来记账。

```bash
sudo cp scripts/health_monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now health_monitor

systemctl status health_monitor
journalctl -u health_monitor -f
```

> ⚠ **路径陷阱**：systemd 下没有 `SUDO_USER`，且 `$HOME` 是 `/root`。
> 程序默认的状态目录解析会因此落到 `/root/demo`，和手动 `sudo` 跑出来的统计**分家**。
> 所以 unit 里必须显式设置 `HEALTH_MONITOR_STATE_DIR`（见 unit 文件内注释）。

---

## 数据出口

### 1. TCP `:6666` — Python 客户端

```bash
python3 client/client.py <板子IP> 6666     # 实时采集，自动落盘 client/data/*.jsonl
python3 client/client.py --replay 文件.jsonl --speed 1.0   # 离线回放
```

每次连接生成一个 `data/health_YYYYMMDD_HHMMSS.jsonl`，每条数据完整保留、不丢字段。

### 2. HTTP `:8080` — Web 仪表盘

浏览器访问 `http://<板子IP>:8080`，页面内嵌 Chart.js，每 500ms 拉取 `/api/sensors`
渲染六轴波形 + 心率/血氧数值。

### 3. MQTT — 本地 broker / 华为云 IoTDA

| Topic | 内容 |
|-------|------|
| `health/sensors` | 传感器数据 |
| `health/status` | 板端在线状态（含遗嘱消息） |
| `$oc/devices/{device_id}/sys/properties/report` | 华为云属性上报（仅云端模式） |

云端模式每次上报 2 条消息、30s 间隔，约 5760 条/日，留足免费版 10000 条/日配额余量。

---

## 长时运行保障

项目从"能跑通"推进到"能一直跑"的这一层，是三件事的组合：

### 1. 硬件看门狗兜底（`src/watchdog.c`）

分工设计成：**进程活着 → 进程喂；进程干净退出 → 内核喂；进程活着但卡死 → 没人喂 → 复位**。
最后一条不是靠显式关设备实现的，而是"没人持有 `/dev/watchdog` fd"这个物理事实自然带来的。

三条反直觉事实（均在 `imx2_wdt.c` 里核实过）：

- **看门狗硬件上停不了**：ops 表里没有 `.stop`，"magic close 关掉"不成立。
  但正常退出**不会**误复位 —— 内核会接手补喂（`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED` 的语义）。
- **超时必须 ≤ 128 秒**：`max_hw_heartbeat_ms = 128000`。超限后卡死时内核照样替你喂，
  保护完全失效 —— 用 `#error` 在编译期钉死。
- **停滞判定按真实流逝时间算**（`CLOCK_MONOTONIC`），不按巡检轮数。按轮数算错过一次：
  睡眠参数非法导致自旋，微秒级就把正常线程全判死。

阈值链：心跳停滞 **20s** 判定 → 停止喂狗 → 再等 **30s** → 硬件复位。
20s 的由来是 I2C 总线被拉死时单次读最坏能到 10s 以上。

复位统计同时记两个计数器：`unclean_boots`（靠应用自写标记，不依赖硬件语义，永远可靠）
与 `wd_resets`（靠 `WDIOC_GETBOOTSTATUS` 的 `WDIOF_CARDRESET` 位）—— 前者是主证据，后者是旁证。

### 2. I2C 总线自动恢复（设备树补丁）

详见 **[docs/i2c-bus-recovery.md](docs/i2c-bus-recovery.md)**。

i.MX6ULL 的 `i2c-imx` 驱动**内置**总线恢复（切 GPIO 敲 9 拍 SCL 把从机敲醒），
但设备树没接上 `pinctrl "gpio"` 状态和 `scl-gpios`/`sda-gpios`，
导致一次仲裁丢失就永久报废。补齐三个必要条件后，恢复通路才接通：

```
ioctl(I2C_RDWR) → i2c_imx_xfer() → i2c_imx_start() → i2c_imx_bus_busy()
   ↓ 检出 I2SR_IAL（仲裁丢失）
i2c-imx.c:439  return -EAGAIN   ← 全文件唯一的 -EAGAIN 出处
   ↓ adapter.bus_recovery_info 非空
i2c_recover_bus() → i2c_generic_scl_recovery()
   ↓ 切 pinctrl 到 gpio 态，敲 9 拍 SCL，每拍后读 SDA 判断是否脱困
   ↓ 切回 default 态，重试一次 i2c_imx_start()
```

> **为什么是 9 拍**：一个字节 = 8 数据位 + 1 ACK 位。卡在字节中间的从机最多再需
> 9 个时钟就能完成该字节、识别 STOP 并释放 SDA（I2C 规范 UM10204 §3.1.16 总线清除）。

### 3. 启动计数与防误判

统计文件路径运行时解析：`HEALTH_MONITOR_STATE_DIR` → sudo 下用 `SUDO_USER` 查家目录 → `$HOME` → `./`。

> **记账时机反直觉**："上次怎么退出的"是在**程序下一次启动时**才判定并写进日志的 ——
> 复位期间没有代码在跑，日志不可能记录自己。所以不自启动的话，复位后 `cat` 到的还是旧快照。

---

## 目录结构

```
health_monitor/
├── src/                        # C 源码（10 个 .c，约 4200 行）
│   ├── main.c                  # 主入口：初始化 + 线程编排 + 带超时 join
│   ├── watchdog.c              # 硬件看门狗 + 复位计数 + 启动日志
│   ├── max30102.c              # MAX30102 驱动 + PPG 算法
│   ├── mpu6050.c               # MPU6050 驱动 + 物理量转换
│   ├── i2c_utils.c             # I2C 读写封装（互斥锁 + 重试 + errno 分类）
│   ├── tcp_server.c            # TCP 工具函数（非阻塞 / 客户端管理）
│   ├── http_server.c           # libmicrohttpd + 内嵌 HTML
│   ├── mqtt_client.c           # libmosquitto 客户端（本地 / 云端双模式）
│   ├── display.c               # 终端渲染
│   └── globals.c               # 全局变量 + 信号处理 + 日志环形缓冲
├── include/                    # 头文件（11 个 .h）
│   ├── config.h                # 全部可调参数集中地
│   ├── watchdog.h              # 看门狗接口 + 被监视槽位定义
│   └── cloud_secret.h.example  # 云平台三元组模板（真实值不入库）
├── client/
│   ├── client.py               # TCP 客户端：接收 + 落盘 JSONL + 离线回放
│   └── data/                   # 落盘数据（.gitignore 忽略）
├── docs/
│   ├── PROJECT_GUIDE.md        # 架构、编译、部署、设计决策
│   └── i2c-bus-recovery.md     # i2c-0 总线瘫痪复盘（内核源码定位 + 面试 Q&A）
├── scripts/
│   ├── health_monitor.service  # systemd unit 参考副本
│   ├── check_i2c.sh            # 板端 I2C 设备检查
│   └── soak_watch.sh           # 长跑采样（RSS / fd 数 / 线程数）
└── Makefile
```

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [docs/PROJECT_GUIDE.md](docs/PROJECT_GUIDE.md) | 详细架构、线程模型、编译部署、关键设计决策、已知限制 |
| [docs/i2c-bus-recovery.md](docs/i2c-bus-recovery.md) | **i2c-0 总线瘫痪完整复盘**：`errno=11` 溯源、恢复调用链、设备树补丁、5 个坑、面试 Q&A |

---

## 已知限制

- **Ctrl+C 偶发失效（未闭环，已暂避）**：个别情况下 `SIGINT` 无法让进程退出，
  已排除"总线瘫痪是唯一原因"（有一次 1 小时无瘫痪但 Ctrl+C 仍不灵）。
  **从未抓到卡住时的内核栈**，这是最该补的一步（`wchan` + `/proc/PID/task/*/stack`）。
  头号嫌疑是 `i2c_transfer` 里拿 `adap->bus_lock` 的 `__mutex_lock`。
  现已由 systemd 托管 + 看门狗兜底绕过，后续复现再定位。
- **MAX30102 中断实际未启用**：INT 引脚开漏输出但硬件未接上拉电阻，INT 恒为低，
  中断不可用，主循环靠 `poll` 50ms 超时轮询 FIFO 兜底。补上拉电阻后可真正启用中断驱动。
- **MPU6050 连续异常后的软复位无退避**：当前每 0.42s 就往死总线上再捅一刀，
  应改为指数退避 + 总线故障标志。**注意**：退避导致的停滞不能超过 20s 心跳阈值，
  两处必须一起设计，否则总线抖一下就把板子喂重启。
- **MPU6050 用 `usleep` 轮询**，实际周期 = 50ms + 读取耗时，长期有累积漂移。
  要精确采样率应改用 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` + 绝对时刻。
- **心率检测用固定阈值**（`PPG_PEAK_THRESH=800`），非自适应，个体差异与运动噪声会误检。
- **SpO2 为经验公式**（`-45.06R² + 30.354R + 94.845`），**非医疗器械级校准，不可用于医疗用途**。
- **JSON 拼装重复**：TCP / HTTP / MQTT 三处结构几乎相同，可抽成 `build_json()`。

---

## 说明

个人学习项目，用于串联嵌入式 Linux 应用开发的主要知识点
（I2C 子系统、多线程、信号、I/O 多路复用、网络、交叉编译、设备树、内核源码排查）。
代码与文档中的分析均基于本板实际内核 **4.19.35-imx6** 源码。
