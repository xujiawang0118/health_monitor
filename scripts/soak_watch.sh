#!/bin/bash
# soak_watch.sh — 健康监测终端长跑采样脚本
#
# 用法：
#   1. 启动程序并把 PID 写入 /tmp/soak.pid（或作为 $1 传入 PID）
#   2. 运行本脚本，每分钟采样 RSS / fd 数 / 线程数
#   3. fd 数量变化时，额外 dump 当前 fd 目标 + TCP 连接，定位开合的是哪类 fd
#
# 指标表写入 /tmp/soak_metrics.log（同时打印到终端）。

PID="${1:-$(cat /tmp/soak.pid 2>/dev/null)}"
LOG="${2:-/tmp/soak_metrics.log}"

if [ -z "$PID" ] || ! kill -0 "$PID" 2>/dev/null; then
    echo "用法: $0 <PID> [日志文件]"
    echo "     或先把 PID 写入 /tmp/soak.pid 再运行 $0"
    exit 1
fi

echo "time        rss_kb  fds  threads" | tee "$LOG"

prev_fds=""
while kill -0 "$PID" 2>/dev/null; do
    rss=$(ps -o rss= -p "$PID")
    fds=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
    thr=$(ls /proc/$PID/task 2>/dev/null | wc -l)
    printf "%s  %s  %s  %s\n" "$(date +%H:%M:%S)" "$rss" "$fds" "$thr" | tee -a "$LOG"

    # fd 数量变化时，dump fd 目标 + TCP 连接，定位开合的 fd 是什么
    if [ -n "$prev_fds" ] && [ "$fds" != "$prev_fds" ]; then
        {
            echo "----- fd $prev_fds -> $fds @ $(date +%H:%M:%S) -----"
            ls -l /proc/$PID/fd 2>/dev/null | awk '{print $9, $10, $11}'
            echo "----- TCP 连接 -----"
            ss -tnp 2>/dev/null | grep "pid=$PID" || echo "(无 TCP 连接)"
            echo "-----------------------------------------------"
        } | tee -a "$LOG"
    fi
    prev_fds="$fds"
    sleep 60
done

echo "[$(date +%H:%M:%S)] 进程退出，采样结束" | tee -a "$LOG"
