/**
 * @file    mpu6050.h
 * @brief   MPU6050 6 轴传感器驱动（I2C 用户空间）
 *
 * MPU6050 内部集成了 3 轴加速度计 + 3 轴陀螺仪 + 温度传感器。
 * 自带 DMP（Digital Motion Processor）可以做姿态解算，
 * 但 Phase 1 只读取原始数据，不过度深入 DMP。
 *
 * 数据手册关键信息：
 * - I2C 地址：0x68（AD0 引脚接 GND）/ 0x69（AD0 接 VCC）
 * - 加速度计量程：±2/4/8/16g 可配置
 * - 陀螺仪量程：±250/500/1000/2000 °/s 可配置
 * - 数据寄存器为 16 位大端（big-endian），需手动拼接和转义
 *
 * 寄存器模型：
 * - 配置寄存器：PWR_MGMT1, GYRO_CONFIG, ACCEL_CONFIG 等
 * - 数据寄存器：ACCEL_XOUT_H/L ~ GYRO_ZOUT_H/L（连续 14 字节）
 * - WHO_AM_I (0x75)：上电默认值 0x68，用于验证 I2C 通信是否正常
 */

#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>

/* ─── I2C 地址 ─── */
#define MPU6050_DEFAULT_ADDR    0x68    /* AD0=GND */
#define MPU6050_ADDR_ALT        0x69    /* AD0=VCC */

/* ─── 寄存器地址 ─── */
/* 配置寄存器 */
#define MPU6050_REG_WHO_AM_I    0x75    /* 芯片 ID，默认 0x68 */
#define MPU6050_REG_PWR_MGMT1   0x6B    /* 电源管理 1 */
#define MPU6050_REG_GYRO_CONFIG 0x1B    /* 陀螺仪量程配置 */
#define MPU6050_REG_ACCEL_CONFIG 0x1C   /* 加速度计量程配置 */
#define MPU6050_REG_SMPLRT_DIV  0x19    /* 采样率分频器 */

/* 数据寄存器（从 ACCEL_XOUT_H 开始连续 14 字节） */
#define MPU6050_REG_ACCEL_XOUT_H 0x3B   /* 加速度 X 轴高字节 */
/* 后续寄存器依次为：
 *   ACCEL_XOUT_L, ACCEL_YOUT_H, ACCEL_YOUT_L,
 *   ACCEL_ZOUT_H, ACCEL_ZOUT_L, TEMP_OUT_H,   TEMP_OUT_L,
 *   GYRO_XOUT_H,  GYRO_XOUT_L,  GYRO_YOUT_H,  GYRO_YOUT_L,
 *   GYRO_ZOUT_H,  GYRO_ZOUT_L
 * 共 14 字节，可以一次性读回。 */

/* ─── 量程配置值（写入对应 CONFIG 寄存器的 bit[4:3]） ─── */
/* 加速度计：±2g(默认), ±4g, ±8g, ±16g */
#define MPU6050_ACCEL_RANGE_2G   0x00
#define MPU6050_ACCEL_RANGE_4G   0x08
#define MPU6050_ACCEL_RANGE_8G   0x10
#define MPU6050_ACCEL_RANGE_16G  0x18

/* 陀螺仪：±250°/s(默认), ±500, ±1000, ±2000 */
#define MPU6050_GYRO_RANGE_250   0x00
#define MPU6050_GYRO_RANGE_500   0x08
#define MPU6050_GYRO_RANGE_1000  0x10
#define MPU6050_GYRO_RANGE_2000  0x18

/* ─── 灵敏度（LSB 对应的物理量，用于将原始值转为实际单位） ─── */
/*
 * 例如加速度计设为 ±2g 时，满量程 = 4g（正负范围），16 位 ADC = 32768 LSB，
 * 所以灵敏度 = 4g / 65536 = 1g / 16384 LSB，即除以 16384 得到以 g 为单位的值。
 */
#define MPU6050_ACCEL_SENS_2G    16384.0f
#define MPU6050_ACCEL_SENS_4G    8192.0f
#define MPU6050_ACCEL_SENS_8G    4096.0f
#define MPU6050_ACCEL_SENS_16G   2048.0f

#define MPU6050_GYRO_SENS_250    131.0f    /* LSB / (°/s) */
#define MPU6050_GYRO_SENS_500    65.5f
#define MPU6050_GYRO_SENS_1000   32.8f
#define MPU6050_GYRO_SENS_2000   16.4f

/* ─── 数据结构 ─── */

/**
 * @brief 传感器原始数据（寄存器的原始值）
 *
 * accel 单位：LSB，除以对应灵敏度得到 g
 * gyro  单位：LSB，除以对应灵敏度得到 °/s
 * temp  单位：LSB，需用公式转为 °C
 */
typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t temp;         /* 温度传感器原始值 */
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} mpu6050_raw_data_t;

/**
 * @brief 传感器物理量数据（已转换为实际单位）
 */
typedef struct {
    float accel_x_g;      /* 加速度，单位 g */
    float accel_y_g;
    float accel_z_g;
    float temp_c;         /* 温度，单位 °C */
    float gyro_x_dps;     /* 角速度，单位 °/s (degree per second) */
    float gyro_y_dps;
    float gyro_z_dps;
} mpu6050_data_t;

/* ─── API ─── */

/**
 * @brief 初始化 MPU6050
 *
 * 执行步骤：
 * 1. 读取 WHO_AM_I 寄存器验证通信
 * 2. 唤醒设备（退出睡眠模式）
 * 3. 配置加速度计和陀螺仪量程
 *
 * @param fd        I2C 文件描述符
 * @param dev_addr  MPU6050 的 I2C 地址
 * @return int      成功返回 0，失败返回 -1
 */
int mpu6050_init(int fd, uint8_t dev_addr);

/**
 * @brief 读取 MPU6050 原始数据
 *
 * 从 ACCEL_XOUT_H 开始连续读取 14 字节，解析为原始值。
 *
 * 注意：
 * - 每次调用会发起一次 I2C 事务，耗时约几百微秒
 * - 不要在一次循环里多次调用这个函数——一次拿全量数据就够
 *
 * @param fd        I2C 文件描述符
 * @param dev_addr  MPU6050 的 I2C 地址
 * @param raw       输出，存放原始数据
 * @return int      成功返回 0，失败返回 -1
 */
int mpu6050_read_raw(int fd, uint8_t dev_addr, mpu6050_raw_data_t *raw);

/**
 * @brief 将原始数据转换为物理量
 *
 * 为什么分两步（read_raw → convert）而不是直接在驱动里转换：
 * - 分离关注点：读取只做 IO，转换只做数学
 * - 不同量程下灵敏度不同，调用者可以根据初始化时的配置传入正确的灵敏度
 *
 * @param raw           原始数据指针
 * @param data          输出，转换后的物理量
 * @param accel_sens    加速度灵敏度常数
 * @param gyro_sens     陀螺仪灵敏度常数
 */
void mpu6050_convert(const mpu6050_raw_data_t *raw, mpu6050_data_t *data,
                     float accel_sens, float gyro_sens);

/**
 * @brief 获取当前配置的加速度灵敏度（默认使用 ±2g）
 */
static inline float mpu6050_get_accel_sensitivity(uint8_t range_config)
{
    switch (range_config) {
        case MPU6050_ACCEL_RANGE_4G:  return MPU6050_ACCEL_SENS_4G;
        case MPU6050_ACCEL_RANGE_8G:  return MPU6050_ACCEL_SENS_8G;
        case MPU6050_ACCEL_RANGE_16G: return MPU6050_ACCEL_SENS_16G;
        default:                       return MPU6050_ACCEL_SENS_2G;
    }
}

/**
 * @brief 获取当前配置的陀螺仪灵敏度（默认使用 ±250°/s）
 */
static inline float mpu6050_get_gyro_sensitivity(uint8_t range_config)
{
    switch (range_config) {
        case MPU6050_GYRO_RANGE_500:  return MPU6050_GYRO_SENS_500;
        case MPU6050_GYRO_RANGE_1000: return MPU6050_GYRO_SENS_1000;
        case MPU6050_GYRO_RANGE_2000: return MPU6050_GYRO_SENS_2000;
        default:                       return MPU6050_GYRO_SENS_250;
    }
}

#endif /* MPU6050_H */
