#!/bin/bash
# ============================================================
# run_on_board.sh — 板端启动 health_ui
# ============================================================
#
# 前置条件（和 speaker_client 那套一致）：
#   1. 守护进程 health_monitor 已经在跑，socket 路径与这里一致
#   2. 当前用户已在 video / input 组里（读写 /dev/fb0 和触摸节点）
#   3. 野火自带的 ebf-qtdemo 已停掉，否则会和本程序抢 framebuffer
#
# 环境变量全部照搬 speaker_client 的验证结果：
#   linuxfb + tslib 触摸 + 关掉 linuxfb 自带的 evdev 输入
# ============================================================

set -u

# ========== 修改点：二进制就在脚本同级目录，无build子文件夹 ==========
APP_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_BIN="$APP_DIR/health_ui"

# systemd 下 HOME 不一定等于 /home/debian，给个兜底，避免 set -u 直接退出
PLUGIN_DIR="${HOME:-/home/debian}/qtplugins"

# 和守护进程 config.h LOCAL_SOCK_PATH 完全一致：/home/debian/demo/health_monitor.sock
SOCK_PATH="${HEALTH_MONITOR_SOCK:-/home/debian/demo/health_monitor.sock}"

if [ ! -x "$APP_BIN" ]; then
    echo "找不到可执行文件: $APP_BIN" >&2
    echo "请在 health_ui/ 目录执行 qmake && make" >&2
    exit 1
fi

# ─── 动态查找 Goodix 触摸节点 ───
# 不能写死 /dev/input/event2：换内核或热插拔后编号会变。
#
# 开机自启时必须等：multi-user.target 到了不代表 udev 已经把输入设备
# 枚举完，触摸驱动是模块时尤其明显。脚本一旦在这一步退出，
# systemd 就会按 RestartSec 反复重启，日志里只剩一句"找不到设备"。
WAIT_SECS=20
EVENT=""
waited=0
while [ "$waited" -lt "$WAIT_SECS" ]; do
    for d in /sys/class/input/event*; do
        if [ "$(cat "$d/device/name" 2>/dev/null)" = "Goodix Capacitive TouchScreen" ]; then
            EVENT="/dev/input/$(basename "$d")"
            break
        fi
    done
    [ -n "$EVENT" ] && break
    sleep 1
    waited=$((waited + 1))
done

if [ -z "$EVENT" ]; then
    echo "找不到 Goodix 触摸设备，检查 usermod -aG input 是否生效" >&2
    exit 1
fi
echo "触摸设备: $EVENT"

# ─── 只借用 tslib 插件，不要把整个插件目录塞进 QT_PLUGIN_PATH ───
# 指向 /usr/lib/plugins 会把野火的 platformtheme 拉进来，
# 那套 Qt 没启用 fontconfig，界面文字会整片消失。
mkdir -p "$PLUGIN_DIR/generic"
if [ ! -f "$PLUGIN_DIR/generic/libqtslibplugin.so" ]; then
    cp /usr/lib/plugins/generic/libqtslibplugin.so "$PLUGIN_DIR/generic/"
fi

pkill -x health_ui 2>/dev/null || true
sleep 0.3

export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export QT_PLUGIN_PATH="$PLUGIN_DIR"
export QT_QPA_GENERIC_PLUGINS="tslib:$EVENT"
export QT_QPA_FB_DISABLE_INPUT=1     # 关键：否则 evdevtouch 和 tslib 打架，按钮点不响

export TSLIB_TSDEVICE="$EVENT"
export TSLIB_CONFFILE=/etc/ts.conf
export TSLIB_CALIBFILE=/etc/pointercal

echo "socket: $SOCK_PATH"

cd "$APP_DIR" || exit 1
exec ./health_ui --socket "$SOCK_PATH"
