#!/bin/bash
# ============================================================
# I2C 设备检查脚本
# 在 i.MX6ULL 板端执行，用于排查传感器通信问题
#
# 用法：
#   chmod +x scripts/check_i2c.sh
#   sudo ./scripts/check_i2c.sh [总线编号]
#
# 默认检查 i2c-0（本项目的传感器总线），也可以指定其他总线：
#   sudo ./scripts/check_i2c.sh 1
#
# 注意命名陷阱：设备树里这条总线叫 &i2c1（0x021a0000），但 Linux 里它是
# /dev/i2c-0，错位一个。所以这里默认值是 0，不是 1。
# ============================================================

set -e  # 任何命令失败则退出

BUS_NUM="${1:-0}"             # 默认 i2c-0
I2C_DEV="/dev/i2c-${BUS_NUM}"

echo "=========================================="
echo "  I2C 总线检查工具"
echo "  总线: I2C-${BUS_NUM}"
echo "  时间: $(date)"
echo "=========================================="
echo ""

# ─── 1. 检查设备节点是否存在 ───
echo "[1/4] 检查设备节点 ${I2C_DEV} ..."
if [ -c "${I2C_DEV}" ]; then
    echo "  ✓ ${I2C_DEV} 存在"
    ls -la "${I2C_DEV}"
else
    echo "  ✗ ${I2C_DEV} 不存在"
    echo ""
    echo "  可能原因："
    echo "    - I2C 控制器未在内核设备树中使能"
    echo "    - 检查 /sys/class/i2c-adapter/ 目录"
    ls -la /sys/class/i2c-adapter/ 2>/dev/null || echo "    (无任何 I2C 适配器)"
    exit 1
fi
echo ""

# ─── 2. 检查 i2c-tools 是否安装 ───
echo "[2/4] 检查 i2c-tools ..."
if command -v i2cdetect &>/dev/null; then
    echo "  ✓ i2c-tools 已安装"
else
    echo "  ✗ i2cdetect 未找到"
    echo "  正在尝试安装..."
    apt-get update -qq && apt-get install -y -qq i2c-tools 2>/dev/null || {
        echo "  ✗ 自动安装失败，请手动执行：sudo apt install i2c-tools"
        exit 1
    }
fi
echo ""

# ─── 3. 扫描 I2C 总线上的设备 ───
echo "[3/4] 扫描 I2C-${BUS_NUM} 总线 ..."
echo ""
i2cdetect -y "${BUS_NUM}"
echo ""
echo "  期望看到的设备地址："
echo "    0x14 → GT1151（板载电容触摸屏，也是 i2c-0 仲裁丢失的重点嫌疑）"
echo "    0x57 → MAX30102（心率血氧）"
echo "    0x68 → MPU6050（六轴传感器）"
echo "    0x69 → MPU6050（如果 AD0 接了 VCC）"
echo ""
echo "  若 0x57/0x68 突然全部消失、且 dmesg 里有 I2C 报错，"
echo "  多半是总线被从机拉死（仲裁丢失），见 docs/i2c-bus-recovery.md"
echo ""

# ─── 4. 检查内核驱动日志 ───
echo "[4/4] 内核 I2C 相关日志（最近 5 行）..."
dmesg 2>/dev/null | grep -i i2c | tail -5 || echo "  (无法读取 dmesg，可能需要 sudo)"
echo ""

# ─── 汇总 ───
echo "=========================================="
echo "  检查完成"
echo "=========================================="
echo ""
echo "如果设备未出现在扫描结果中，排查步骤："
echo "  1. 确认硬件接线：VCC→3.3V, GND→GND, SCL→SCL, SDA→SDA"
echo "  2. 检查是否有上拉电阻（I2C 通常需要 4.7kΩ 上拉到 3.3V）"
echo "  3. 用万用表测量 VCC-GND 之间的电压是否为 3.3V"
echo "  4. 换一组杜邦线试试（线材接触不良很常见）"
echo "  5. 上电后触摸传感器，看是否正常发热（短路/开路判断）"
