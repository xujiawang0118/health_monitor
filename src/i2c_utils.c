/**
 * @file    i2c_utils.c
 * @brief   I2C 操作封装 —— 读用 ioctl(I2C_RDWR)，写用 write()
 *
 * Linux i2c-dev 驱动提供两种用户空间 I2C 访问方式：
 *
 *   方式 A: ioctl(I2C_SLAVE) + write()/read()
 *     - 每次 read()/write() 是独立的 I2C 事务（中间有 STOP）
 *     - 读寄存器 = write(reg_addr) 再 read(data)，两次事务
 *
 *   方式 B: ioctl(I2C_RDWR) + i2c_msg 复合事务
 *     - 一次 ioctl 完成"写寄存器地址→重复 START→读数据"
 *     - 地址直接写在 msg.addr 里，不需要 ioctl(I2C_SLAVE)
 *
 * ## 为什么读用方式 B、写用方式 A
 *
 * i.MX6ULL 的 i2c-imx 控制器对"方式 A 的多字节读"不稳定：
 * 间歇性全 0 / -ENXIO / -ETIMEDOUT，高中断负载（断网/上云）时更易触发。
 * 单字节读可靠（所以 probe 读 WHO_AM_I 正常），多字节读（MPU6050 14 字节、
 * MAX30102 FIFO 6 字节）要改成方式 B 的一次复合事务才稳。
 * 写是单事务、一向可靠，保持方式 A 即可。
 */

#include <stdio.h>        /* snprintf, perror, fprintf */
#include <stdlib.h>       /* NULL */
#include <unistd.h>       /* close, read, write, usleep */
#include <fcntl.h>        /* open, O_RDWR */
#include <sys/ioctl.h>    /* ioctl */
#include <string.h>       /* memcpy */
#include <errno.h>        /* errno */
#include <pthread.h>     /* pthread_mutex_t, pthread_mutex_lock/unlock */
#include <signal.h>      /* sig_atomic_t */
#include <linux/i2c.h>      /* I2C_M_RD, struct i2c_msg */
#include <linux/i2c-dev.h>  /* I2C_SLAVE, I2C_RDWR, struct i2c_rdwr_ioctl_data */

#include "i2c_utils.h"

/*
 * 程序级运行标志（定义在 globals.c）：为 0 表示收到退出信号。
 * 这里只读，用于在重试循环里尽早收手——总线被拉死时单次 ioctl 可能
 * 阻塞数百毫秒甚至更久，关机阶段不该再傻等 5 次重试。
 * i2c_utils 本应是通用封装，但本项目只有 health_monitor 用它，
 * 直接引用这个全局标志换取更快的优雅退出，可接受。
 */
extern volatile sig_atomic_t g_running;

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
 * 注意：本函数由 i2c_write_reg 内部调用（读走 I2C_RDWR 不经过它），
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

    /*
     * 用 ioctl(I2C_RDWR) 构造一次复合事务：
     *   msg[0]: 写 1 字节寄存器地址（flags=0，普通写）
     *   msg[1]: 读 len 字节（flags=I2C_M_RD）
     * 两者之间是重复起始（repeated START）而非 STOP——对 i2c-imx 控制器
     * 而言这是一次原子读事务，比 write(reg) 停一下再 read(data) 稳得多。
     *
     * 地址直接写在 msg.addr 里，不需要先 ioctl(I2C_SLAVE)。
     */
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data rdwr;

    msgs[0].addr  = dev_addr;
    msgs[0].flags = 0;                 /* 写：发寄存器地址 */
    msgs[0].len   = 1;
    msgs[0].buf   = &reg_addr;

    msgs[1].addr  = dev_addr;
    msgs[1].flags = I2C_M_RD;          /* 读：重复起始 + 读 N 字节 */
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    rdwr.msgs  = msgs;
    rdwr.nmsgs = 2;

    int retries;
    for (retries = I2C_MAX_RETRIES; retries > 0; retries--) {
        /* 收到退出信号：停止重试，让线程尽快退出（关机阶段不做慢速 I/O） */
        if (!g_running) {
            pthread_mutex_unlock(&g_i2c_lock);
            return -1;
        }

        /*
         * 关键：ioctl(I2C_RDWR) 失败时，内核 i2c_transfer 可能已经改写了
         * msgs[1].len（记录已传输字节数）和 msgs[1].buf（指针前移）。
         * 重试前必须复位，否则下一次读的字节数和目标地址是错的。
         */
        msgs[1].len = len;
        msgs[1].buf = buf;

        /* 成功时 ioctl 返回"处理完成的消息数"，这里应为 2 */
        if (ioctl(fd, I2C_RDWR, &rdwr) == 2) {
            pthread_mutex_unlock(&g_i2c_lock);
            return 0;
        }

        usleep(I2C_RETRY_DELAY_US);
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
        /* 收到退出信号：停止重试 */
        if (!g_running) {
            pthread_mutex_unlock(&g_i2c_lock);
            return -1;
        }

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
