/**
 * @file    i2c_utils.c
 * @brief   I2C 操作封装 —— 使用 write()/read() 方式
 *
 * ## 为什么用 write()/read() 而不是 ioctl(I2C_RDWR)
 *
 * Linux i2c-dev 驱动提供了两种用户空间 I2C 访问方式：
 *
 *   方式 A: ioctl(I2C_SLAVE) + write()/read()
 *     - 自 Linux 2.6 以来一直支持，兼容性最好
 *     - 每次 read()/write() 是一个独立的 I2C 事务（有 STOP）
 *     - 对于简单寄存器读取：先 write(reg_addr) 再 read(data)
 *       无法做到一次事务完成"写寄存器地址→读数据"（需要重复 START）
 *     - 但大多数 I2C 设备对这种模式没有意见
 *
 *   方式 B: ioctl(I2C_RDWR) + i2c_msg 复合事务
 *     - 一次 ioctl 完成"写寄存器地址→重 START→读数据"
 *     - 效率更高，但部分老内核（如本板子的 4.19）可能有兼容问题
 *
 * 本项目采样率仅 20Hz，方式 A 的性能完全足够。
 * 参考：野火官方 MPU6050 demo 使用的就是方式 A。
 */

#include <stdio.h>        /* snprintf, perror, fprintf */
#include <stdlib.h>       /* NULL */
#include <unistd.h>       /* close, read, write, usleep */
#include <fcntl.h>        /* open, O_RDWR */
#include <sys/ioctl.h>    /* ioctl */
#include <string.h>       /* memcpy */
#include <errno.h>        /* errno */
#include <pthread.h>     /* pthread_mutex_t, pthread_mutex_lock/unlock */
#include <linux/i2c-dev.h>    /* I2C_SLAVE */

#include "i2c_utils.h"

/* ─── 重试次数 ─── */
/*
 * I2C 通信偶尔会因为电磁干扰等原因出现单次错误，
 * 加上重试机制可以提高可靠性。
 * 官方 MPU6050 demo 使用了 5 次重试，这里保持一致。
 *
 * 注意：内核头文件 <linux/i2c-dev.h> 中已经 #define 了 I2C_RETRIES (0x0701)，
 * 所以用 I2C_MAX_RETRIES 避免命名冲突。
 */
#define I2C_MAX_RETRIES      5
#define I2C_RETRY_DELAY_US   10000   /* 10ms，官方 demo 使用的间隔 */


//全局锁
static pthread_mutex_t g_i2c_lock = PTHREAD_MUTEX_INITIALIZER;


/* ─── 内部：设置从设备地址 ─── */
/*
 * ioctl(I2C_SLAVE, addr) 的作用：
 *   1. 如果 file->private_data 为 NULL，创建一个 i2c_client
 *   2. 将 i2c_client->addr 设置为指定的 7 位地址
 *   3. 后续的 read()/write() 系统调用会使用这个 client 来寻址
 *
 * 必须使用 7 位地址（如 0x68），不是左移后的 8 位地址（0xD0）。
 *
 * ## 缓存机制
 *
 * 在 i.MX6ULL 4.19 内核上，反复调用 I2C_SLAVE ioctl（即使地址没变）
 * 会触发 i2c_client 的内部状态重置，导致后续 read()/write() 返回
 * EAGAIN（errno=11）。缓存当前地址，地址未变时跳过 ioctl，避免此问题。
 */
static int current_slave = -1;  /* -1 表示尚未设置 */

/*
 * 注意：本函数由 i2c_read_reg / i2c_write_reg 内部调用，
 * 调用者已经持有 g_i2c_lock，此处不再加锁。
 */
static int i2c_set_slave(int fd, uint8_t dev_addr)
{
    /* 地址没变，跳过 ioctl——避免触发 i.MX I2C 驱动的状态重置 */
    if(current_slave == (int)dev_addr)
    {
        return 0;
    }
    if(ioctl(fd, I2C_SLAVE,(unsigned long)dev_addr) < 0)
    {
        perror("i2c_set_slave: ioctl(I2C_SLAVE) 失败");
        return -1;
    }
    current_slave = (int)dev_addr;
    return 0;
}

/* ─── public ─── */

int i2c_open(int bus_num)
{
    pthread_mutex_lock(&g_i2c_lock);
    char path[32];
    snprintf(path,sizeof(path),"/dev/i2c-%d",bus_num);
    int fd = open(path,O_RDWR);
    if(fd < 0)
    {
        
        perror("i2c_open: 打开 I2C 设备失败");
        pthread_mutex_unlock(&g_i2c_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_i2c_lock);
    return fd;
}

void i2c_close(int fd)
{
    pthread_mutex_lock(&g_i2c_lock);
    if (fd >= 0) {
        close(fd);
    }
    pthread_mutex_unlock(&g_i2c_lock);
}

int i2c_read_reg(int fd, uint8_t dev_addr, uint8_t reg_addr,
                 uint8_t *buf, size_t len)
{
    pthread_mutex_lock(&g_i2c_lock);
    if (fd < 0 || buf == NULL || len == 0) {
        pthread_mutex_unlock(&g_i2c_lock);
        return -1;
    }

    /* 步骤 1：设置从设备地址 */
    if (i2c_set_slave(fd, dev_addr) < 0) {
        pthread_mutex_unlock(&g_i2c_lock);
        return -1;
    }

    /*
     * 步骤 2：write(寄存器地址) → read(数据)
     *
     * 说明：write() 和 read() 各是一次独立的 I2C 事务，中间会发 STOP。
     * 对于 MPU6050 这类传感器，先 write 寄存器地址再 read 数据
     * 是完全可以的——设备会保持寄存器指针。
     *
     * 重试机制：I2C 通信偶尔出现单次错误（干扰、时钟抖动），
     * 重试几次通常能恢复。
     */
    int retries;
    for (retries = I2C_MAX_RETRIES; retries > 0; retries--) {
        /*
         * write() 在 i2c-dev 驱动中的处理路径：
         *   i2cdev_write() → i2c_master_send(client, buf, count)
         * 发送一次完整的 I2C 写事务：START + addr(W) + data + STOP
         */
        if (write(fd, &reg_addr, 1) != 1) {
            usleep(I2C_RETRY_DELAY_US);
            continue;
        }

        /*
         * read() 在 i2c-dev 驱动中的处理路径：
         *   i2cdev_read() → i2c_master_recv(client, buf, count)
         * 发送一次完整的 I2C 读事务：START + addr(R) + data + STOP
         */
        if (read(fd, buf, len) != (ssize_t)len) {
            usleep(I2C_RETRY_DELAY_US);
            continue;
        }

        /* write + read 都成功 */
        pthread_mutex_unlock(&g_i2c_lock);
        return 0;
    }

    fprintf(stderr,
            "i2c_read_reg(0x%02X, reg=0x%02X): "
            "%d 次重试全部失败，最后一次 errno=%d (%s)\n",
            dev_addr, reg_addr, I2C_MAX_RETRIES, errno, strerror(errno));
    pthread_mutex_unlock(&g_i2c_lock);
    return -1;
}

int i2c_write_reg(int fd, uint8_t dev_addr, uint8_t reg_addr,
                  const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&g_i2c_lock);
    if (fd < 0 || data == NULL || len == 0) {
        pthread_mutex_unlock(&g_i2c_lock);
        return -1;
    }

    /* 步骤 1：设置从设备地址 */
    if (i2c_set_slave(fd, dev_addr) < 0) {
        pthread_mutex_unlock(&g_i2c_lock);
        return -1;
    }

    /*
     * 步骤 2：write(寄存器地址 + 数据)
     *
     * 把 reg_addr 和 data 拼在一个连续缓冲区中一次 write() 发出。
     * I2C 协议中，写寄存器就是：
     *   START + addr(W) + reg + data[0] + data[1] + ... + STOP
     * 后面的字节由设备自动递增寄存器指针。
     *
     * 栈上分配 VLA，对于本项目几十字节完全足够。
     */
    uint8_t buffer[len + 1];
    buffer[0] = reg_addr;
    memcpy(buffer + 1, data, len);

    int retries;
    for (retries = I2C_MAX_RETRIES; retries > 0; retries--) {
        if (write(fd, buffer, len + 1) == (ssize_t)(len + 1)) {
            pthread_mutex_unlock(&g_i2c_lock);
            return 0;
        }
        usleep(I2C_RETRY_DELAY_US);
    }

    fprintf(stderr,
            "i2c_write_reg(0x%02X, reg=0x%02X): "
            "%d 次重试全部失败，最后一次 errno=%d (%s)\n",
            dev_addr, reg_addr, I2C_MAX_RETRIES, errno, strerror(errno));
    pthread_mutex_unlock(&g_i2c_lock);
    return -1;
}

int i2c_write_byte(int fd, uint8_t dev_addr, uint8_t reg_addr,
                   uint8_t value)
{
    return i2c_write_reg(fd, dev_addr, reg_addr, &value, 1);
}
