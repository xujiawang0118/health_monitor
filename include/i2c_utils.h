/**
 * @file    i2c_utils.h
 * @brief   Linux 用户空间 I2C 操作封装
 *
 * 本模块对 Linux I2C 设备节点（/dev/i2c-N）的读写操作做了基础封装，
 * 采用 ioctl(I2C_SLAVE) 设置从设备地址 + write()/read() 方式。
 *
 * 选择 write()/read() 而非 ioctl(I2C_RDWR) 复合事务的原因：
 * - 兼容性最好，i.MX6ULL 4.19 内核上 I2C_RDWR 可能有兼容问题
 * - 20Hz 采样率下性能完全足够
 * 详细设计说明见 i2c_utils.c 文件头注释。
 */

#ifndef I2C_UTILS_H
#define I2C_UTILS_H

#include <stdint.h>   /* uint8_t, uint16_t */
#include <stddef.h>   /* size_t */

/**
 * @brief 打开 I2C 设备节点
 *
 * @param bus_num   I2C 总线编号，如 1 对应 /dev/i2c-1
 * @return int      成功返回文件描述符(>=0)，失败返回 -1
 *
 * 使用示例：
 *   int fd = i2c_open(1);
 *   if (fd < 0) { perror("i2c_open"); }
 */
int i2c_open(int bus_num);

/**
 * @brief 关闭 I2C 设备
 *
 * @param fd    i2c_open() 返回的文件描述符
 */
void i2c_close(int fd);

/**
 * @brief 从 I2C 设备的指定寄存器连续读取多个字节
 *
 * 底层执行两次独立 I2C 事务：
 *   1. write(寄存器地址)  → 一次完整的写事务（含 STOP）
 *   2. read(N 字节)       → 一次完整的读事务（含 STOP）
 *
 * @param fd        I2C 文件描述符
 * @param dev_addr  7 位 I2C 从设备地址（如 0x68，不要传左移后的 8 位地址）
 * @param reg_addr  要读取的起始寄存器地址
 * @param buf       输出缓冲区，读取到的数据存入此处（调用者分配内存）
 * @param len       期望读取的字节数
 * @return int      成功返回 0，失败返回 -1（errno 由内核设置）
 *
 * 为什么传 dev_addr 而不在 open 时指定：
 *   同一个 I2C 总线上可以挂多个设备，地址是跟着每次事务走的。
 */
int i2c_read_reg(int fd, uint8_t dev_addr, uint8_t reg_addr,
                 uint8_t *buf, size_t len);

/**
 * @brief 向 I2C 设备的指定寄存器写入数据
 *
 * 底层执行的是 I2C 写事务：
 *   1. 主机写 1 字节（寄存器地址）
 *   2. 主机写 N 字节（要写入的数据）   → 发 STOP
 *
 * @param fd        I2C 文件描述符
 * @param dev_addr  7 位 I2C 从设备地址
 * @param reg_addr  目标寄存器地址
 * @param data      要写入的数据
 * @param len       数据长度（字节）
 * @return int      成功返回 0，失败返回 -1
 */
int i2c_write_reg(int fd, uint8_t dev_addr, uint8_t reg_addr,
                  const uint8_t *data, size_t len);

/**
 * @brief 向 I2C 设备写入单个字节（常用场景：写控制寄存器）
 *
 * 这是 i2c_write_reg 的便捷包装。
 *
 * @param fd        I2C 文件描述符
 * @param dev_addr  7 位 I2C 从设备地址
 * @param reg_addr  目标寄存器地址
 * @param value     写入的 1 字节值
 * @return int      成功返回 0，失败返回 -1
 */
int i2c_write_byte(int fd, uint8_t dev_addr, uint8_t reg_addr,
                   uint8_t value);

#endif /* I2C_UTILS_H */
