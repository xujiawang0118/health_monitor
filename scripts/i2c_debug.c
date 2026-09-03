/**
 * @file    i2c_debug.c
 * @brief   最小 I2C_RDWR 测试——隔离排查 ioctl 失败原因
 *
 * 编译（板端）：
 *   gcc -o i2c_debug i2c_debug.c -Wall
 *
 * 运行：
 *   sudo ./i2c_debug /dev/i2c-0 0x68 0x75
 *
 * 预期输出：读到的值 = 0x68（MPU6050 的 WHO_AM_I）
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

/* 不依赖外部头文件，直接定义所需的结构体和常量 */

/*
 * i2c_msg 结构体（来自 <linux/i2c.h>）
 * 这里手动定义，避免和工具链的内核头文件版本冲突
 */
struct i2c_msg {
    unsigned short addr;   /* 从设备地址（已左移 1 位） */
    unsigned short flags;  /* 0 = 写, 1 = 读 */
#define I2C_M_RD 0x0001
    unsigned short len;    /* 消息字节数 */
    unsigned char  *buf;   /* 数据缓冲区指针 */
};

/*
 * i2c_rdwr_ioctl_data 结构体（来自 <linux/i2c-dev.h>）
 */
struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;  /* 消息数组指针 */
    unsigned int nmsgs;    /* 消息数量 */
};

/*
 * I2C_RDWR ioctl 命令码
 * 来自 <linux/i2c-dev.h>：
 * #define I2C_RDWR _IOWR('i', 0x07, struct i2c_rdwr_ioctl_data)
 *
 * _IOWR 的结果 = 0x80000000 | (0x69 << 8) | 0x07 | (sizeof(struct i2c_rdwr_ioctl_data) << 16)
 * sizeof(struct i2c_rdwr_ioctl_data) = 8 (在 32 位 ARM 上)
 *
 * 命令码 = 0x80086907
 *
 * 但为了安全，直接用 _IOWR 宏算出正确值，
 * 同时打印出来确认和系统头文件是否一致。
 */
#include <asm/ioctl.h>
#define I2C_RDWR _IOWR('i', 0x07, struct i2c_rdwr_ioctl_data)

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <i2c_dev> <dev_addr> <reg_addr>\n", argv[0]);
        fprintf(stderr, "示例: %s /dev/i2c-0 0x68 0x75\n", argv[0]);
        return 1;
    }

    const char *dev_path = argv[1];
    unsigned int dev_addr_7bit = strtoul(argv[2], NULL, 0);
    unsigned int reg_addr = strtoul(argv[3], NULL, 0);

    printf("=== I2C 最小测试 ===\n");
    printf("设备节点:   %s\n", dev_path);
    printf("从设备地址: 0x%02X (7-bit)\n", dev_addr_7bit);
    printf("寄存器地址: 0x%02X\n", reg_addr);
    printf("ioctl 命令码: 0x%08lX\n", (unsigned long)I2C_RDWR);
    printf("\n");

    /* 1. 打开 I2C 设备 */
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    printf("[OK] open(%s, O_RDWR) → fd=%d\n", dev_path, fd);

    /* 2. 构造 I2C 事务：写 1 字节寄存器地址 + 读 1 字节数据 */
    unsigned char reg = (unsigned char)reg_addr;
    unsigned char val = 0xFF;  /* 初始化为非 0x68 的值，方便确认是否真的读到了 */

    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data rdwr;

    memset(msgs, 0, sizeof(msgs));

    /* 消息 0：写寄存器地址 */
    msgs[0].addr  = (unsigned short)(dev_addr_7bit << 1);  /* 7 位地址 → 8 位格式 */
    msgs[0].flags = 0;       /* 写 */
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    /* 消息 1：读 1 字节 */
    msgs[1].addr  = (unsigned short)(dev_addr_7bit << 1);
    msgs[1].flags = I2C_M_RD; /* 读 */
    msgs[1].len   = 1;
    msgs[1].buf   = &val;

    /* 打印消息详情 */
    printf("msgs[0]: addr=0x%04X flags=%u len=%u buf→reg=0x%02X\n",
           msgs[0].addr, msgs[0].flags, msgs[0].len, reg);
    printf("msgs[1]: addr=0x%04X flags=%u len=%u buf→&val\n",
           msgs[1].addr, msgs[1].flags, msgs[1].len);

    rdwr.msgs  = msgs;
    rdwr.nmsgs = 2;
    printf("rdwr: nmsgs=%u, msgs=%p\n", rdwr.nmsgs, (void *)rdwr.msgs);
    printf("sizeof(i2c_msg)=%zu sizeof(rdwr)=%zu\n",
           sizeof(struct i2c_msg), sizeof(struct i2c_rdwr_ioctl_data));

    /* 3. 执行 ioctl */
    printf("\n>>> ioctl(fd=%d, I2C_RDWR=0x%08lX, &rdwr)\n", fd, (unsigned long)I2C_RDWR);
    int ret = ioctl(fd, I2C_RDWR, &rdwr);
    if (ret < 0) {
        printf("[FAIL] ioctl 返回 %d, errno=%d (%s)\n", ret, errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("[OK] ioctl 成功，返回 %d\n", ret);
    printf("读取到的值: 0x%02X (期望 0x68 或 0x98)\n", val);

    /* 4. 多次测试，排除偶然性 */
    printf("\n--- 连续 5 次读取测试 ---\n");
    for (int i = 0; i < 5; i++) {
        val = 0xFF;
        msgs[0].buf = &reg;
        msgs[1].buf = &val;
        ret = ioctl(fd, I2C_RDWR, &rdwr);
        if (ret < 0) {
            printf("  [%d] FAIL: errno=%d (%s)\n", i+1, errno, strerror(errno));
        } else {
            printf("  [%d] val=0x%02X\n", i+1, val);
        }
    }

    close(fd);
    return 0;
}
