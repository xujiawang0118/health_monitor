#ifndef MAX30102_H
#define MAX30102_H

#include <stdint.h>
#include <stdbool.h>

/* ─── MAX30102 I2C 7 位从设备地址 ─── */
#define MAX30102_DEV_ADDR    0x57

/* ─── 寄存器地址 ─── */
#define REG_MODE_CFG    0x09   /* 模式配置（复位/SHDN/工作模式） */
#define REG_FIFO_WR_PTR 0x04   /* FIFO 写指针 */
#define REG_FIFO_RD_PTR 0x06   /* FIFO 读指针 */
#define REG_FIFO_DATA   0x07   /* FIFO 数据寄存器（读时自动递增） */
#define REG_FIFO_CFG    0x08   /* FIFO 配置（平均次数/满标志/循环） */
#define REG_FIFO_OVF_CNT 0x05  /* FIFO 溢出计数器 */
#define REG_SPO2_CFG    0x0A   /* SpO2 配置（ADC量程/采样率/LED脉宽） */
#define REG_LED1_PA     0x0C   /* LED1（红光）电流 */
#define REG_LED2_PA     0x0D   /* LED2（红外）电流 */
#define REG_PART_ID     0xFF   /* 产品 ID（应为 0x15） */
#define REG_INTR_ENABLE_1 0x02 /* 中断使能寄存器 1 */

/* ─── PPG 算法参数 ─── */
#define PPG_WINDOW_SIZE  128     /* DC 滑动窗口大小：@100Hz=1.28s，覆盖完整心跳周期 */
#define ADC_MASK_18BIT   0x3FFFF /* ADC 有效 18bit 掩码 */
#define PPG_PEAK_THRESH_MIN 100  /* 自适应峰值阈值下限，防止无信号时噪声误触发 */

/* ─── FIFO ─── */
#define MAX30102_FIFO_DEPTH  32  /* FIFO 深度（最多存 32 个 sample） */

//max30102中断引脚
#define CHIP_PATH   "/dev/gpiochip2"
#define MAX30102_INT    27


/* ─── 数据结构 ─── */

/* 一组原始红光+红外数据（一个 FIFO sample = 6 字节） */
typedef struct {
    uint32_t red;   /* 红光 ADC 值（18bit 有效） */
    uint32_t ir;    /* 红外 ADC 值（18bit 有效） */
} PPG_RawData_t;

/* DC 基线（静态）与 AC 脉搏分量（交流） */
typedef struct {
    float red_dc;      /* 红光 DC 基线（窗口均值） */
    float ir_dc;       /* 红外 DC 基线（窗口均值） */
    float red_ac;      /* 红光瞬时 AC = raw - DC（心率峰值检测用，逐 sample 变化） */
    float ir_ac;       /* 红外瞬时 AC = raw - DC（心率峰值检测用，逐 sample 变化） */
    float red_ac_pp;   /* 红光峰峰值 AC = 窗口 max - min（血氧 R 比值用，稳定） */
    float ir_ac_pp;    /* 红外峰峰值 AC = 窗口 max - min（血氧 R 比值用，稳定） */
} PPG_ACDC_t;

/* PPG 最终输出 */
typedef struct {
    uint8_t heart_rate;   /* 心率（bpm） */
    uint8_t spo2;         /* 血氧饱和度（%） */
    bool    data_valid;   /* 数据有效标志 */
} PPG_Result_t;

/* ─── 函数声明 ─── */

/* 初始化 MAX30102（复位→清 FIFO→配置参数→进入 SpO2 模式） */
int max30102_init(int fd, uint8_t dev_addr);

/* 初始化 MAX30102 中断引脚，返回 event fd 供 poll() 使用 */
int max30102_int_init(void);

/* 读取并清除 GPIO 中断事件（poll 返回后必须调用） */
int max30102_int_read_event(void);

/* 释放 MAX30102 中断引脚资源 */
void max30102_int_cleanup(void);

/* 查询 FIFO 中可读的 sample 数量（0~32），失败返回 -1 */
int max30102_get_fifo_count(int fd, uint8_t dev_addr);

/* 从 FIFO 读取一个 sample（6 字节），写入 raw 指向的结构体 */
int max30102_read_fifo(int fd, uint8_t dev_addr, PPG_RawData_t *raw);

/* 原始数据 → 心率/血氧（统一处理入口） */
PPG_Result_t ppg_process(PPG_RawData_t raw, uint64_t sys_ms);

/* 查询 DC 窗口已填充的样本数（0 ~ PPG_WINDOW_SIZE） */
int ppg_get_fill_count(void);

#endif
