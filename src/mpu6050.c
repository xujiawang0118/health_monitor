/**
 * @file    mpu6050.c
 * @brief   MPU6050 驱动 —— 实现文件
 *
 * ## 初始化流程
 *
 * 1. 读取 WHO_AM_I (0x75)  → 确认返回 0x68，I2C 通信正常
 * 2. 写 PWR_MGMT1 (0x6B)   → bit6(DEVICE_RESET) 清 0，
 *    bit6(CYCLE) 清 0，bit5(TEMP_DIS) 清 0，
 *    最终写入 0x00 退出睡眠模式，选择内部 8MHz 振荡器
 * 3. 写 GYRO_CONFIG (0x1B)  → 配置陀螺仪量程
 * 4. 写 ACCEL_CONFIG (0x1C) → 配置加速度计量程
 *
 * ## 数据读取
 *
 * 所有传感器数据从 ACCEL_XOUT_H (0x3B) 开始，连续 14 字节：
 *
 *   Offset  Content
 *   ------  -------
 *   0x3B    ACCEL_XOUT[15:8]  高字节
 *   0x3C    ACCEL_XOUT[7:0]   低字节
 *   0x3D    ACCEL_YOUT[15:8]
 *   0x3E    ACCEL_YOUT[7:0]
 *   0x3F    ACCEL_ZOUT[15:8]
 *   0x40    ACCEL_ZOUT[7:0]
 *   0x41    TEMP_OUT[15:8]
 *   0x42    TEMP_OUT[7:0]
 *   0x43    GYRO_XOUT[15:8]
 *   0x44    GYRO_XOUT[7:0]
 *   0x45    GYRO_YOUT[15:8]
 *   0x46    GYRO_YOUT[7:0]
 *   0x47    GYRO_ZOUT[15:8]
 *   0x48    GYRO_ZOUT[7:0]
 *
 * ## 数据拼接与补码
 *
 * 每个轴的值 = (high_byte << 8) | low_byte
 * 结果是有符号 16 位整数（int16_t），符号位在 high_byte 的 bit7。
 * C 语言的类型转换：uint8_t → uint16_t → int16_t 会自动处理符号。
 * 简化做法：直接 (int16_t)((high << 8) | low)
 *
 * ## 温度转换公式
 *
 * 手册公式：Temperature (°C) = (TEMP_OUT / 340.0) + 36.53
 * TEMP_OUT 是原始有符号 16 位值。
 * 这个温度是芯片内部温度，不是环境温度，精度约 ±1°C。
 */

#include <stdio.h>        /* fprintf, stderr */
#include <unistd.h>       /* usleep */
#include "i2c_utils.h"
#include "mpu6050.h"

int mpu6050_init(int fd, uint8_t dev_addr)
{
    uint8_t who_am_i = 0;
    int ret;

    /* ── 步骤 1：验证 WHO_AM_I ── */
    /*
     * 这是最基本的 I2C 通信检查。如果读不到 0x68（或 0x98，不同批次
     * 寄存器的 bit6 可能不同），说明：
     * - 接线有问题（SDA/SCL 接反、没接上拉电阻）
     * - I2C 地址不对
     * - 设备未上电
     */
    ret = i2c_read_reg(fd,dev_addr,MPU6050_REG_WHO_AM_I,&who_am_i,1);
    if(ret < 0)
    {
        perror("mpu6050_init: 读取 WHO_AM_I 失败");
        return -1;
    }
    if (who_am_i != 0x68) {
        fprintf(stderr, "mpu6050_init: WHO_AM_I 返回 0x%02X，期望 0x68\n", who_am_i);
        return -1;
    }


    /* ── 步骤 2：唤醒设备 ── */
    /*
     * PWR_MGMT1 寄存器：
     *   bit7: H_RESET (写 1 复位所有寄存器，随后自动清 0)
     *   bit6: SLEEP   (1=睡眠，0=正常工作)
     *   bit5: CYCLE   (1=循环模式，与 SLEEP 配合)
     *   bit3: TEMP_DIS (1=禁用温度传感器)
     *   bit[2:0]: CLKSEL (时钟源选择，0=内部 8MHz 振荡器)
     *
     * 写入 0x00 = 退出睡眠 + 不循环 + 温度传感器使能 + 内部时钟
     */
    ret = i2c_write_byte(fd, dev_addr, MPU6050_REG_PWR_MGMT1, 0x00);
    if (ret < 0) {
        perror("mpu6050_init: 写 PWR_MGMT1 失败");
        return -1;
    }


    /*
     * 退出睡眠后需要等待芯片内部振荡器稳定。
     * 手册建议上电稳定时间约 30ms，这里稍多一些确保安全。
     */
    usleep(50000);  /* 50ms */
    printf("[MPU6050] 已唤醒，时钟源：内部 8MHz\n");

    /* ── 步骤 3：配置陀螺仪量程 ── */
    /*
     * GYRO_CONFIG bit[4:3]:
     *   00 = ±250°/s
     *   01 = ±500°/s
     *   10 = ±1000°/s
     *   11 = ±2000°/s
     *
     * Phase 1 先用默认 ±250°/s，不修改。
     */
    ret = i2c_write_byte(fd,dev_addr,MPU6050_REG_GYRO_CONFIG,0x00);
    if (ret < 0) {
        perror("mpu6050_init: 写 GYRO_CONFIG 失败");
        return -1;
    }



    /* ── 步骤 4：配置加速度计量程 ── */
    /*
     * ACCEL_CONFIG bit[4:3]:
     *   00 = ±2g
     *   01 = ±4g
     *   10 = ±8g
     *   11 = ±16g
     */
    ret = i2c_write_byte(fd,dev_addr,MPU6050_REG_ACCEL_CONFIG,0x00);
    if (ret < 0) {
        perror("mpu6050_init: 写 ACCEL_CONFIG 失败");
        return -1;
    }

    printf("[MPU6050] 初始化完成：加速度 ±2g，陀螺仪 ±250°/s\n");
    return 0;
}

int mpu6050_read_raw(int fd, uint8_t dev_addr, mpu6050_raw_data_t *raw)
{
    if (raw == NULL) {
        return -1;
    }

    /*
     * 一次读 14 字节。栈上分配的数组，长度固定，不需要动态内存。
     * 连续读取比逐个寄存器读取效率高得多：
     * - 逐寄存器读：14 次 I2C 事务，每次约 0.5ms → 7ms
     * - 连续读：1 次 I2C 事务 → 约 0.5ms
     */
    uint8_t buf[14];
    int ret = i2c_read_reg(fd, dev_addr, MPU6050_REG_ACCEL_XOUT_H, buf, 14);
    if (ret < 0) {
        perror("mpu6050_read_raw: 读取原始数据失败");
        return -1;
    }

    /*
     * 数据拼接。buf 的顺序对应上面注释中的 14 个寄存器，
     * 每个轴的 H 在前 L 在后（大端序）。
     *
     * (int16_t) 强转处理了补码：如果 high_byte 的 bit7 = 1，
     * 说明是负数，转换为 int16_t 时会自动进行符号扩展。
     */
    raw->accel_x = (int16_t)((buf[0]  << 8) | buf[1]);
    raw->accel_y = (int16_t)((buf[2]  << 8) | buf[3]);
    raw->accel_z = (int16_t)((buf[4]  << 8) | buf[5]);
    raw->temp    = (int16_t)((buf[6]  << 8) | buf[7]);
    raw->gyro_x  = (int16_t)((buf[8]  << 8) | buf[9]);
    raw->gyro_y  = (int16_t)((buf[10] << 8) | buf[11]);
    raw->gyro_z  = (int16_t)((buf[12] << 8) | buf[13]);

    return 0;
}

void mpu6050_convert(const mpu6050_raw_data_t *raw, mpu6050_data_t *data,
                     float accel_sens, float gyro_sens)
{
    if (raw == NULL || data == NULL) {
        return;
    }

    /*
     * 加速度：原始 LSB 值 ÷ 灵敏度 = g 值
     * 例如静止平放时 accel_z ≈ 16384 / 16384 = 1.0g（重力加速度）
     */
    data->accel_x_g = (float)raw->accel_x / accel_sens;
    data->accel_y_g = (float)raw->accel_y / accel_sens;
    data->accel_z_g = (float)raw->accel_z / accel_sens;

    /*
     * 陀螺仪：原始 LSB 值 ÷ 灵敏度 = °/s
     * 静止时三轴应接近 0
     */
    data->gyro_x_dps = (float)raw->gyro_x / gyro_sens;
    data->gyro_y_dps = (float)raw->gyro_y / gyro_sens;
    data->gyro_z_dps = (float)raw->gyro_z / gyro_sens;

    /*
     * 温度转换公式（来自 MPU6050 数据手册）：
     *   Temperature (°C) = (TEMP_OUT / 340.0) + 36.53
     *
     * 注意：这是芯片内部温度，通常比环境温度高几度。
     */
    data->temp_c = (float)raw->temp / 340.0f + 36.53f;
}
