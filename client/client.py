#!/usr/bin/env python3
"""
health_monitor 客户端 — 接收传感器 JSON、实时显示、落盘存储、离线回放

用法:
  python3 client.py [IP] [端口]             实时采集（数据自动落盘到 data/）
  python3 client.py --replay 文件.jsonl      离线回放（读取 JSONL 文件重放）

实时模式下，每条接收到的 JSON 会追加写入 data/health_日期_时间.jsonl，
每条数据完整保留，不丢失任何字段。回放时按原始时间戳间隔重现。
"""

import socket
import json
import signal
import sys
import time
import os
import argparse

# ─── 默认配置 ───
# SERVER_IP   = "172.20.10.2"
SERVER_IP   = "192.168.65.130"
SERVER_PORT = 6666
DATA_DIR    = "data"

# ─── 优雅退出 ───
running = True

def sigint_handler(sig, frame):
    global running
    running = False

signal.signal(signal.SIGINT, sigint_handler)

# ─── 命令行解析 ───
parser = argparse.ArgumentParser(description="health_monitor 客户端")
parser.add_argument("ip_or_replay", nargs="?", default=None,
                    help="服务器 IP，或 --replay 时指定 JSONL 文件路径")
parser.add_argument("port", nargs="?", type=int, default=SERVER_PORT,
                    help=f"服务器端口（默认 {SERVER_PORT}）")
parser.add_argument("--replay", "-r", action="store_true",
                    help="离线回放模式")
parser.add_argument("--speed", "-s", type=float, default=1.0,
                    help="回放速度倍率（默认 1.0，2.0 = 两倍速）")
args = parser.parse_args()


# ═══════════════════════════════════════════════════════════
#  方案设计说明
# ═══════════════════════════════════════════════════════════
#
#  存储格式选择 JSONL（JSON Lines）而非 CSV：
#
#  CSV 的问题：
#    - 字段是扁平的，{"mpu": {"ax": 1.0}} 这种嵌套结构需要展开成多列
#    - 后期新增字段（如增加环境光传感器）需要改表头，兼容性差
#    - 数据类型丢失：1 是 int 还是 float？"1.0" vs 1.0 语义不同
#    - 回放时必须反向解析 CSV → dict，再交给 render()，多一层转换
#
#  JSONL 的优势：
#    - 每行就是服务端发出的原始 JSON，原样写入，零转换
#    - 结构无损，render() 直接 json.loads(line) 即可用
#    - 标准格式，pandas.read_json() / jq 都能直接消费
#    - 文件可追加（append），进程重启不丢数据
#
#  文件命名：health_20260721_143025.jsonl
#    - 日期时间来自本机，不是板端时间戳（方便按采集时段查找）
#    - 放在 data/ 目录下，该目录已加入 .gitignore
#
#  回放机制：
#    - 读取 JSONL，逐行解析
#    - 每两条记录之间的间隔 = (下一条的 t_ms - 当前条 t_ms) / speed
#    - 第一条记录立即显示，不等待
#    - Ctrl+C 随时中断回放
# ═══════════════════════════════════════════════════════════


def make_log_path() -> str:
    """生成带时间戳的 JSONL 文件路径"""
    os.makedirs(DATA_DIR, exist_ok=True)
    name = time.strftime("health_%Y%m%d_%H%M%S.jsonl")
    return os.path.join(DATA_DIR, name)


def write_jsonl(f, data: dict) -> None:
    """追加一条 JSON 记录到文件（末尾换行）"""
    # json.dumps 默认不输出空格，体积最小
    line = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    f.write(line + "\n")
    f.flush()  # 确保即使崩溃已写入的数据不丢失


# ──────────────────────────────────────────────
#  终端渲染（实时和回放共用）
# ──────────────────────────────────────────────

def render(d: dict, *, log_path: str = "", frame: int = 0) -> None:
    """用 ANSI 转义刷新终端"""
    mpu = d.get("mpu", {})
    ppg = d.get("ppg", {})

    ts = d.get("t", 0)
    ts_str = time.strftime("%H:%M:%S", time.localtime(ts / 1000.0))

    hr   = ppg.get("hr",  0)
    spo2 = ppg.get("spo2", 0)
    valid = "✓" if ppg.get("valid") else "✗"

    lines = [
        "\033[H\033[J",
        "╔══════════════════════════════════╗",
        "║  健康监测终端 — 客户端          ║",
        "╠══════════════════════════════════╣",
       f"║ 服务端: {SERVER_IP}:{SERVER_PORT:<5}             ║",
       f"║ 更新时间: {ts_str}                ║",
        "╠══════════════════════════════════╣",
        "",
        "─── MPU6050 六轴传感器 ───",
       f"  加速度(g): X={mpu.get('ax',0):+8.3f}  Y={mpu.get('ay',0):+8.3f}  Z={mpu.get('az',0):+8.3f}",
       f"  角速度(°/s): X={mpu.get('gx',0):+8.2f}  Y={mpu.get('gy',0):+8.2f}  Z={mpu.get('gz',0):+8.2f}",
       f"  温度: {mpu.get('temp',0):.1f} °C",
        "",
        "─── MAX30102 血氧/心率 ───",
       f"  心率: {hr:3d} bpm    血氧: {spo2:3d} %    有效: {valid}",
        "",
    ]
    # 实时模式显示日志路径和帧计数，回放模式显示进度
    if log_path:
        lines.append(f"  记录文件: {log_path}")
    if frame > 0:
        lines.append(f"  已记录: {frame} 条")
    lines.append("Ctrl+C 退出")
    print("\n".join(lines), flush=True)


# ═══════════════════════════════════════════════════════════
#  实时采集模式
# ═══════════════════════════════════════════════════════════

def run_live() -> None:
    global SERVER_IP, SERVER_PORT

    if args.ip_or_replay:
        SERVER_IP = args.ip_or_replay
    if args.port:
        SERVER_PORT = args.port

    # 打开日志文件
    log_path = make_log_path()
    log_file = open(log_path, "w", encoding="utf-8")
    # 写入文件头注释（JSONL 标准允许 # 开头的注释行）
    log_file.write(f"# health_monitor 数据记录\n")
    log_file.write(f"# 采集时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    log_file.write(f"# 服务端: {SERVER_IP}:{SERVER_PORT}\n")
    log_file.write(f"# 格式: [\"t\"]=毫秒时间戳, [\"mpu\"]=MPU6050, [\"ppg\"]=MAX30102\n")
    log_file.flush()

    # 连接服务端
    print(f"正在连接 {SERVER_IP}:{SERVER_PORT} ...", end=" ", flush=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((SERVER_IP, SERVER_PORT))
        sock.settimeout(0.5)
    except (ConnectionRefusedError, OSError) as e:
        print(f"失败: {e}")
        log_file.close()
        sys.exit(1)
    print("已连接")

    print("\033[?25l", end="", flush=True)

    buf = b""
    last_data = None
    last_update = 0.0
    frame_count = 0
    conn_lost = False

    try:
        while running:
            try:
                buf += sock.recv(4096)
            except socket.timeout:
                now = time.time()
                if last_data and now - last_update > 0.2:
                    render(last_data, log_path=log_path, frame=frame_count)
                    last_update = now
                continue
            except (ConnectionResetError, BrokenPipeError):
                conn_lost = True
                break

            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    data = json.loads(line.decode("utf-8"))
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue

                # 写入日志文件
                write_jsonl(log_file, data)
                frame_count += 1

                last_data = data
                now = time.time()
                if now - last_update > 0.2:
                    render(data, log_path=log_path, frame=frame_count)
                    last_update = now

    finally:
        sock.close()
        log_file.close()
        print("\033[?25h", end="", flush=True)
        print(f"\n日志已保存到: {log_path} ({frame_count} 条记录)")
        if conn_lost:
            print("[错误] 与服务端的连接已断开")


# ═══════════════════════════════════════════════════════════
#  离线回放模式
# ═══════════════════════════════════════════════════════════

def run_replay() -> None:
    if not args.ip_or_replay:
        print("用法: python3 client.py --replay 文件.jsonl")
        sys.exit(1)

    filepath = args.ip_or_replay
    if not os.path.isfile(filepath):
        print(f"文件不存在: {filepath}")
        sys.exit(1)

    speed = args.speed
    if speed <= 0:
        print("回放速度必须大于 0")
        sys.exit(1)

    print(f"回放模式 | 文件: {filepath} | 速度: {speed}x | Ctrl+C 退出")
    time.sleep(0.5)
    print("\033[?25l", end="", flush=True)

    # 先读取所有有效行（跳过注释）
    records = []
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue

    if not records:
        print("文件中没有有效数据")
        print("\033[?25h", end="", flush=True)
        sys.exit(1)

    frame_count = len(records)
    last_ts = None
    idx = 0

    try:
        for i, d in enumerate(records):
            if not running:
                break
            render(d, log_path=filepath, frame=i + 1)

            # 按原始时间间隔等待
            cur_ts = d.get("t", 0)
            if last_ts is not None and cur_ts > last_ts:
                delay = (cur_ts - last_ts) / 1000.0 / speed
                if delay > 0:
                    time.sleep(delay)
            last_ts = cur_ts
            idx = i

    finally:
        print("\033[?25h", end="", flush=True)
        print(f"\n回放完成: {idx + 1}/{frame_count} 条记录")


# ═══════════════════════════════════════════════════════════
#  入口
# ═══════════════════════════════════════════════════════════

if __name__ == "__main__":
    if args.replay:
        run_replay()
    else:
        run_live()
