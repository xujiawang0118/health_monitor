# health_ui — 板端 Qt 界面

守护进程（`health_monitor`）的第 8 个线程开了一个 AF_UNIX 服务端，
本工程用 `QLocalSocket` 连上去，只做两件事：

- **读**：接收 2Hz 的传感器快照，刷新数值和心率曲线
- **写**：发 `ping` 之类的命令，按 `seq` 对响应

硬件全在守护进程手里，界面进程崩了不影响采集，重启界面即可重连。

---

## 文件结构

```
health_ui/
├── health_ui.pro         qmake 工程（FORMS 里挂 .ui，uic 自动生成 ui_mainwindow.h）
├── mainwindow.ui         ★ 界面布局，用 Qt Designer 打开改
├── mainwindow.h/.cpp     MainWindow：业务逻辑，只有少量"塞控件"的代码
├── localclient.h/.cpp    QLocalSocket 封装：重连 + 按行切帧 + seq 分配
└── main.cpp              入口，解析 --socket / 环境变量
```

**边界很明确**：所有布局、控件属性（字体、对齐、间距）都在 `.ui` 里，
`mainwindow.cpp` 只管数据流。想调界面不要动 C++，打开 Designer 改完重编就行。

---

## Qt Designer 工作流

### 打开方式

```bash
designer health_ui/mainwindow.ui
# 或者 Qt Creator 里直接双击 mainwindow.ui
```

### 界面上的四个关键 objectName

`.ui` 和 C++ 之间是靠 **objectName** 联系的，改名等于改接口：

| objectName        | 类型        | 用途                              |
|-------------------|-------------|-----------------------------------|
| `stateLabel`      | QLabel      | 连接状态（未连接 / 已连接 / 数据中断）|
| `pingButton`      | QPushButton | 发 `ping` 命令                    |
| `hrValueLabel`    | QLabel      | 心率数值                          |
| `spo2ValueLabel`  | QLabel      | 血氧数值                          |
| `accelValueLabel` | QLabel      | 三轴加速度                        |
| `chartContainer`  | **普通 QWidget** | 图表的容器                   |
| `statusbar`       | QStatusBar  | 底部提示条                        |

改名前先在 Designer 里想清楚：C++ 里的 `ui->hrValueLabel` 会直接编译不过，
还算好查；但 `on_pingButton_clicked()` 改名后是**静默失效**——编译通过、
点了没反应，这类问题最难找。要么不改名，要么改完顺手改槽名。

### 为什么 `chartContainer` 是普通 QWidget

Designer 里"提升为"（promote to）`QChartView` 看起来更顺，但坑在两个版本差异：

- Qt5 的图表类在 `QtCharts` 命名空间里，头文件是 `<QtCharts/QChartView>`
- Qt6 取消了命名空间，头文件路径也变了

提升的类会被 uic 写死成某一个版本的 include，换 Qt 版本直接编译失败。
所以 `.ui` 里只放一个空 QWidget，`QChartView` 在 `setupChart()` 里 new 出来塞进去，
版本差异用 `#if QT_VERSION < QT_VERSION_CHECK(6,0,0)` 一处吃掉。

### 槽函数自动连接

`ui->setupUi(this)` 末尾会调用 `QMetaObject::connectSlotsByName(this)`，
自动把下面这种签名的槽接到同名控件的信号上：

```cpp
void on_pingButton_clicked();   // on_<objectName>_<signalName>
```

所以 `pingButton` 不需要手写 `connect()`。注意这个机制只认 `<objectName>_<signal>`，
名字拼错不报错。

### 新增一个按钮的完整流程

1. Designer 里拖一个 `QPushButton`，`objectName` 设成 `resetButton`
2. 保存 `.ui`
3. `mainwindow.h` 的 `private slots:` 里加 `void on_resetButton_clicked();`
4. `mainwindow.cpp` 实现它
5. 重新 `make`

`uic` 是构建时自动跑的（`.pro` 里 `FORMS +=` 触发），不需要手动执行。

---

## 构建

### PC 端（开发调试）

PC 上**没有板子也没关系**：在 PC 上跑 mock 版守护进程，两边走同一个 UDS 路径。

```bash
# 终端 1：mock 守护进程（无硬件，假数据）
cd health_monitor
make EXTRA_CFLAGS=-DSENSOR_MOCK
HEALTH_MONITOR_SOCK=/tmp/health.sock ./build_mock/health_monitor

# 终端 2：界面
cd health_ui
qmake health_ui.pro && make
./health_ui --socket /tmp/health.sock       # 或 HEALTH_MONITOR_SOCK=/tmp/health.sock ./health_ui
```

Windows 上写界面时连不到板子（理由见下一节），可以用 Qt 跑一个
"假服务端"：随便一个进程在 `\\.\pipe\xxx`（Windows）或 `/tmp/xxx.sock`（WSL）上
按协议灌 NDJSON 就行。

### 板端（i.MX6ULL / armhf）

```bash
# PC 上交叉编译
/path/to/Qt/5.15.2/gcc_arm/bin/qmake health_ui.pro
make

# 传到板子（本项目统一用 wget + http.server，scp 不可用）
python3 -m http.server 8000
# 板子上：
wget http://<PC_IP>:8000/health_ui
chmod +x health_ui
```

板端运行前确认三件事：

1. **Qt 运行库在板子上**：`health_ui` 是动态链接的，需要把 Qt 的 `lib/`
   一起拷到板子（或设置 `LD_LIBRARY_PATH`）。Qt Charts 对应的
   `libQt5Charts.so.5` 别漏。
2. **路径一致**：守护进程用 `LOCAL_SOCK_PATH`（默认 `/home/debian/demo/health_monitor.sock`），
   Qt 端默认值写死在 `main.cpp` 里，两边保持一致；用环境变量覆盖时两边都要设。
3. **权限**：socket 文件由守护进程创建，`chmod 0660`。
   两个进程必须**同一个用户**或**同组**，否则 `connect()` 直接 EACCES。
   目录也得可写（socket 要在这个目录里创建）。

### 换到有触摸屏的板子

`.pro` 里没有写死平台插件。板端如果只有 framebuffer：

```bash
./health_ui -platform linuxfb      # 无 X11 / Wayland 时
./health_ui -platform eglfs        # 有 GPU 时性能更好
```

Qt 的 `linuxfb` 插件对 i.MX6ULL 这类没有 3D 加速的板子够用，
但注意它不会自动处理触摸校准。

---

## 通信协议（与板端 `local_server.c` 对齐）

**字节流 + 按 `\n` 切帧的 NDJSON**，一行一个 JSON 对象。

### 下行（守护进程 → 界面）

连接建立时立刻推一帧，之后每 500ms 一帧：

```json
{"t":1761372000123,"mpu":{"ax":0.02,"ay":-0.01,"az":1.00,"gx":0.5,"gy":-0.3,"gz":0.1,"temp":31.2},"ppg":{"hr":72,"spo2":98,"valid":1}}
```

| 字段 | 说明 |
|------|------|
| `t` | **CLOCK_REALTIME** 毫秒（不是单调时钟），板子 NTP 对时会往回跳 |
| `mpu` | 加速度单位 g，角速度单位 °/s，温度 °C |
| `ppg.valid` | `1`/`0`（cJSON 把 bool 写成了数字），0 表示手指没放上去或还在预热 |
| `ppg.hr` / `ppg.spo2` | `valid=0` 时这两个值没有意义 |

### 上行（界面 → 守护进程）

```json
{"seq":1,"cmd":"ping"}
```

响应：

```json
{"seq":1,"ok":true}
{"seq":1,"ok":false,"err":"unknown cmd"}
```

`seq` 由客户端自己分配，服务端原样回填。界面侧用 `QHash<seq, 时间>` 记录，
2 秒没等到响应就判定超时——**超时必须有**，否则守护进程卡在 I2C 里时
界面按钮会永远灰着。

---

## 代码里几个必须知道的点

### 1. `canReadLine()` 而不是自己攒缓冲

`QLocalSocket` 继承 `QIODevice`，内部自带读缓冲。一帧被拆成两次
`readyRead` 是常态（内核 socket 缓冲区边界和写入边界无关）：

```cpp
while (m_socket->canReadLine()) {          // 缓冲里确实有 '\n' 才返回 true
    QByteArray line = m_socket->readLine();
    ...
}
```

C 侧（`local_server.c`）没有这层便利，必须自己维护 `inbuf + inlen + memmove`，
两边做的事一样，只是一个有库帮忙。

### 2. 断线重连要防重复

`errorOccurred` 和 `disconnected` 可能同时来，都调 `scheduleReconnect()`
会堆出多个定时器。`LocalClient` 里用 `m_wantConnected` +
`m_reconnectTimer->isActive()` 两道闸门挡住。

重连用 `abort()` 而不是 `disconnectFromServer()`：`abort()` 顺带清空内部缓冲，
避免残留半帧导致协议错位——这种错位排查起来比断线难十倍。

### 3. `socket 活着 ≠ 数据在流`

这两个状态必须分开显示：

- `disconnected` → "连接断开，重连中…"（红）
- 3 秒没收到快照 → "数据中断"（黄）

传感器线程卡在 I2C 里时，UDS 还好好连着，只有靠停滞计时器才能发现。

### 4. 曲线点数上上限

2Hz、300 点 ≈ 150 秒可视窗口，超过就丢最老的：

```cpp
if (m_hrSeries->count() >= 300) m_hrSeries->remove(0);
```

不限的话跑一晚上，series 里就是十万个点，板子上会越来越卡。

### 5. X 轴用时间戳，不用索引

`QDateTimeAxis` 吃毫秒时间戳。用真实时间的好处是断连几分钟后，
曲线中间会自己留一段空白，而不是把缺口压平看不出问题。

但要防 NTP 回跳：`t` 倒退超过 1 秒就清空重画（`clearSeries()`），
否则轴的区间 `[t0, t1]` 会翻转。

### 6. AF_UNIX 只能本机

这是设计约束，不是 bug：UDS 不经过网络栈，PC 上的 Qt 连不到板子的 socket 文件。
PC 端要看板子实时数据，走两条现成的路：

- 现有 TCP `:6666`（协议是另一套，要写 `QTcpSocket` 版客户端）
- 浏览器看 `http://<板子IP>:8080` 的 Chart.js 面板

---

## 常见故障对照

| 现象 | 原因 |
|------|------|
| `connectToServer` 报 `Connection refused`，文件存在 | 守护进程没起，或上次被 SIGKILL 留下陈旧 socket（守护进程启动时会 `lstat`+`unlink`） |
| 报 `server not found` | 路径不一致（一端设了 `HEALTH_MONITOR_SOCK`，另一端没设） |
| `Permission denied` | socket 属主/权限不对，两个进程不同用户且不同组 |
| 连上但一直"数据中断" | 守护进程的 `t` 字段没更新 → 传感器线程卡死，看板端日志 |
| 曲线画不出来 | `t` 回跳；或 Y 轴区间被极端值撑爆（已做钳位） |
| 板子上窗口全黑 | 平台插件不对，试 `-platform linuxfb` |
