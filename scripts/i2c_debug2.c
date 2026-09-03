/**
 * @file    i2c_debug2.c
 * @brief   用 SMBus 方式验证 I2C 通信 — 作为 i2c_utils 的备选实现路径
 *
 * 编译（板端）：
 *   gcc -o i2c_debug2 i2c_debug2.c -Wall
 *
 * 运行：
 *   sudo ./i2c_debug2 /dev/i2c-0 0x68 0x75
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

/*
 * SMBus 数据结构（来自 <linux/i2c.h>）
 *
 * 手动定义，避免头文件版本冲突。
 * 这些结构和常量自 Linux 2.6 以来没有变化。
 */

/* I2C 从设备地址设置 */
#define I2C_SLAVE       0x0703  /* 正常模式：内核必须没占用该地址 */
#define I2C_SLAVE_FORCE 0x0706  /* 强制模式：即使内核驱动占用了也允许通信 */

/* SMBus 事务类型 */
#define I2C_SMBUS_READ  1
#define I2C_SMBUS_WRITE 0

/* SMBus 数据大小 */
#define I2C_SMBUS_BYTE      0
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3

/* SMBus 数据联合体 */
union i2c_smbus_data {
    unsigned char  byte;
    unsigned short word;
    unsigned char  block[34];  /* block[0] = length */
};

/* SMBus ioctl 参数 */
struct i2c_smbus_ioctl_data {
    char read_write;          /* I2C_SMBUS_READ 或 I2C_SMBUS_WRITE */
    unsigned char command;    /* 寄存器地址（SMBus 术语中叫 command） */
    int size;                 /* I2C_SMBUS_BYTE_DATA 等 */
    union i2c_smbus_data *data;
};

/*
 * I2C_SMBUS ioctl 命令码计算
 * _IOWR('i', 0x09, struct i2c_smbus_ioctl_data)
 * sizeof(struct i2c_smbus_ioctl_data) =
 *   1 (char) + 1 (unsigned char) + 2 (padding) + 4 (int) + 4 (pointer) = 12 on ARM32
 * 等等，让我计算：
 *   char read_write:         1 byte  (offset 0)
 *   unsigned char command:   1 byte  (offset 1)
 *   padding:                 2 bytes (offset 2-3)
 *   int size:                4 bytes (offset 4-7)
 *   pointer data:            4 bytes (offset 8-11)
 *   Total: 12 bytes
 *
 * _IOWR('i', 0x09, 12) = 0xC00C6909
 */
#include <asm/ioctl.h>
#define I2C_SMBUS _IOWR('i', 0x09, struct i2c_smbus_ioctl_data)


int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <i2c_dev> <dev_addr_7bit> <reg_addr>\n", argv[0]);
        fprintf(stderr, "示例: %s /dev/i2c-0 0x68 0x75\n", argv[0]);
        return 1;
    }

    const char *dev_path = argv[1];
    unsigned int dev_addr = strtoul(argv[2], NULL, 0);
    unsigned int reg_addr = strtoul(argv[3], NULL, 0);

    printf("=== I2C SMBus 测试 ===\n");
    printf("sizeof(i2c_smbus_ioctl_data) = %zu\n",
           sizeof(struct i2c_smbus_ioctl_data));
    printf("I2C_SMBUS ioctl 码 = 0x%08lX\n", (unsigned long)I2C_SMBUS);
    printf("I2C_SLAVE_FORCE        = 0x%08lX\n", (unsigned long)I2C_SLAVE_FORCE);
    printf("\n");

    /* 1. 打开设备 */
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    printf("[OK] open → fd=%d\n", fd);

    /* 2. 设置从设备地址 */
    printf(">>> ioctl(fd, I2C_SLAVE_FORCE, 0x%02X)\n", dev_addr);
    if (ioctl(fd, I2C_SLAVE_FORCE, dev_addr) < 0) {
        printf("[FAIL] 无法设置从设备地址: errno=%d (%s)\n",
               errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("[OK] 从设备地址已设置\n");

    /* 3. 用 SMBus 读取一个寄存器字节 */
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data smbus;

    memset(&data, 0, sizeof(data));
    smbus.read_write = I2C_SMBUS_READ;
    smbus.command    = (unsigned char)reg_addr;
    smbus.size       = I2C_SMBUS_BYTE_DATA;
    smbus.data       = &data;

    printf("SMBus: cmd=0x%02X read_write=%d size=%d\n",
           smbus.command, smbus.read_write, smbus.size);

    printf(">>> ioctl(fd, I2C_SMBUS=0x%08lX, &smbus)\n", (unsigned long)I2C_SMBUS);
    if (ioctl(fd, I2C_SMBUS, &smbus) < 0) {
        printf("[FAIL] SMBus ioctl: errno=%d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    printf("[OK] 读到: 0x%02X (期望 0x68)\n", data.byte);

    /* 4. 连读 5 次 */
    printf("\n--- 连续 5 次读取 ---\n");
    for (int i = 0; i < 5; i++) {
        memset(&data, 0, sizeof(data));
        smbus.command = (unsigned char)reg_addr;
        smbus.data = &data;
        if (ioctl(fd, I2C_SMBUS, &smbus) < 0) {
            printf("  [%d] FAIL: %s\n", i+1, strerror(errno));
        } else {
            printf("  [%d] 0x%02X\n", i+1, data.byte);
        }
    }

    close(fd);
    printf("\n=== 测试完成 ===\n");
    return 0;
}
