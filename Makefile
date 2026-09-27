# ============================================================
# Makefile — 健康监测终端
# ============================================================
#
# 支持四种编译模式：
#   1. make              → 使用系统 gcc（PC 端测试编译用）
#   2. make ASAN=1       → 使用系统 gcc + AddressSanitizer（PC 端内存检查用）
#   3. make cross        → 使用交叉编译工具链（板端运行）
#   4. make clean        → 清理构建产物
#
# ============================================================

# ─── 项目名称 ───
TARGET  := health_monitor

# ─── 目录结构 ───
SRC_DIR   := src
INC_DIR   := include

# ─── 链接选项（先定义基础库，ASan 在后面按需追加） ───
# 本地链接：动态链接（PC 端 glibc 版本一致，无此问题）
# 需要 libmicrohttpd-dev / libmosquitto-dev / libssl-dev / libgpiod-dev 开发包
LDFLAGS_LOCAL := -lpthread -lm -lmicrohttpd -lmosquitto -lssl -lcrypto -lgpiod

# 交叉链接：静态链接，避免板端 glibc 版本不匹配
#   板端 Debian 10: glibc 2.28
#   工具链(系统):  glibc 2.40+
#   静态链接后二进制自包含 ~700KB，可接受
#   注意：libmicrohttpd / libmosquitto / libssl / libcrypto / libgpiod 的 armhf .a 文件需提前准备
LDFLAGS_CROSS := -lpthread -lm -lmicrohttpd -lmosquitto -lssl -lcrypto -lgpiod -static

# ─── 构建目录 + 编译选项（ASan 可选，仅 PC 本地编译） ───
# AddressSanitizer：make ASAN=1 开启，产物输出到独立 build_asan/ 目录，
# 与普通 build/ 互不干扰，切换无需手动 clean。
#   -fsanitize=address      内存错误检测（越界/use-after-free/泄漏）
#   -fno-omit-frame-pointer 保留栈帧指针，报错时能定位到具体函数
#   -O0                     关闭优化，报错行号准确
ifdef ASAN
  BUILD_DIR := build_asan
  CFLAGS    := -Wall -Wextra -O0 -g -I$(INC_DIR) -std=gnu11
  CFLAGS    += -fsanitize=address -fno-omit-frame-pointer
  LDFLAGS_LOCAL += -fsanitize=address
else
  BUILD_DIR := build
  CFLAGS    := -Wall -Wextra -O2 -g -I$(INC_DIR) -std=gnu11
endif

# ─── 源文件（自动收集 src/ 下所有 .c 文件） ───
SRCS  := $(wildcard $(SRC_DIR)/*.c)
OBJS  := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# ─── 编译器选择 ───
# 本地编译：直接使用系统 gcc
CC_LOCAL  := gcc

# 交叉编译：野火 i.MX6ULL 使用 arm-linux-gnueabihf-gcc
CC_CROSS  := arm-linux-gnueabihf-gcc

# 默认用本地 gcc
CC := $(CC_LOCAL)

# 默认 LDFLAGS 用本地
LDFLAGS := $(LDFLAGS_LOCAL)

# ============================================================
# 编译目标
# ============================================================

.PHONY: all clean cross check help

# ─── 默认目标 ───
all: $(BUILD_DIR)/$(TARGET)

# ─── 交叉编译目标（固定输出到 build/，不受 ASAN 影响） ───
cross:
	@echo "[INFO] 交叉编译器: $(CC_CROSS)"
	@echo "[INFO] 链接方式: 静态链接"
	@$(MAKE) all CC="$(CC_CROSS)" LDFLAGS="$(LDFLAGS_CROSS)" BUILD_DIR=build

# ─── 链接 ───
$(BUILD_DIR)/$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	@echo "[LD] $@"
	@$(CC) $^ -o $@ $(LDFLAGS)
	@echo ""
	@echo "============================================"
	@echo "  编译完成: $(BUILD_DIR)/$(TARGET)"
	@echo "  编译器:   $(CC)"
	@echo "============================================"
	@echo ""

# ─── 编译 .c → .o ───
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	@echo "[CC] $< → $@"
	@$(CC) -c $< -o $@ $(CFLAGS)

# ─── 清理 ───
clean:
	@echo "[CLEAN] 删除 build/ build_asan/ build_cross/"
	@rm -rf build build_asan build_cross
	@echo "[CLEAN] 完成"

# ─── I2C 设备检查（板端执行） ───
check:
	@echo "=== I2C 总线检查 ==="
	@ls -la /dev/i2c-* 2>/dev/null || echo "无 I2C 设备节点"
	@echo ""
	@echo "=== 扫描 I2C-1 总线上的设备 ==="
	@i2cdetect -y 1 2>/dev/null || echo "请先安装 i2c-tools: sudo apt install i2c-tools"
	@echo ""
	@echo "=== 内核 I2C 驱动加载情况 ==="
	@dmesg | grep -i i2c 2>/dev/null | tail -5 || echo "无法读取 dmesg（可能需要 sudo）"

# ─── 帮助 ───
help:
	@echo "可用目标："
	@echo "  make          - 使用系统 gcc 编译（PC 端测试用）"
	@echo "  make ASAN=1   - 使用 gcc + AddressSanitizer 编译（PC 端内存检查）"
	@echo "  make cross    - 使用 arm-linux-gnueabihf-gcc 静态链接交叉编译"
	@echo "  make check    - 在板端检查 I2C 设备（需要在板子上运行）"
	@echo "  make clean    - 删除编译产物"
	@echo "  make help     - 显示本信息"
