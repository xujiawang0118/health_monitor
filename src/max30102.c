/**
 * @file    max30102.c
 * @brief   MAX30102 心率血氧传感器驱动 + PPG 信号处理
 *
 * ## 传感器工作原理（简述）
 *
 * MAX30102 内部集成红光（660nm）/红外（880nm）LED 和光电二极管。
 * LED 发光穿过皮肤→血管→组织，反射光强随脉搏周期性变化。
 * 光电二极管检测反射光强，ADC 采样后存入 32 深度 FIFO。
 *
 * 信号链路：
 *   LED发光 → 组织反射 → PD检测 → ADC(18bit) → FIFO → I2C读取
 *
 * PPG 信号 = DC 基线（组织/静脉等静态吸收） + AC 分量（动脉搏动）
 * AC/DC 比值用于计算血氧饱和度。
 *
 * ## 心率计算原理
 *
 * 红外光（IR）的 AC 分量对应脉搏波形。检测波形的连续峰值，
 * 两个峰之间的时间间隔（IBI）换算为瞬时心率：
 *   HR = 60000 / IBI_ms
 *
 * 本实现使用简单的固定阈值 + 迟滞检测峰值，非医用级别。
 *
 * ## 血氧计算原理
 *
 * 氧合血红蛋白（HbO2）和脱氧血红蛋白（Hb）对红光和红外光的
 * 吸收率不同。定义 R 比值：
 *   R = (AC_red / DC_red) / (AC_ir / DC_ir)
 * 通过经验公式 R → SpO2。
 */

#include "max30102.h"
#include "i2c_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <gpiod.h>

/* ─── GPIO 中断资源（文件级静态变量，外部不可见） ─── */
/*
 * 设计思路：
 *   max30102_int_init()     初始化 chip/line，返回 event fd 给调用方的 poll()
 *   max30102_int_read_event()   poll 返回后清除 GPIO 事件（内部用静态 line）
 *   max30102_int_cleanup()      释放 GPIO 资源
 *
 *   main.c 只需要持有 event fd 做 poll 和 line 指针做 read，
 *   但 line 指针是 gpiod 内部类型，不应暴露到头文件——
 *   所以 read_event/cleanup 封装在 max30102.c 内部，外部只调函数。
 *
 *   这与 ppg_get_fill_count() 封装 buf_fill_count 是同样的模式。
 */
static struct gpiod_chip *int_chip = NULL;
static struct gpiod_line *int_line = NULL;

/* ─── 全局状态（单线程安全） ─── */

/* 滑动窗口缓存（环形缓冲），用于计算 DC 基线 */
static float red_buf[PPG_WINDOW_SIZE];
static float ir_buf[PPG_WINDOW_SIZE];
static uint8_t buf_idx = 0;
static uint8_t buf_fill_count = 0;  /* 窗口已填充的样本数（满窗后恒为 PPG_WINDOW_SIZE） */

/* 心率峰值检测状态 */
static uint64_t last_peak_ms = 0;
static uint8_t  peak_flag = 0;
static float    ir_ac_peak = 0;   /* 近期 ir_ac 正峰值，用于自适应阈值 */

/* 心率锁存状态：连续心跳期间保持最近一次有效心率，避免两次心跳之间归零 */
static uint8_t  g_heart_rate = 0;      /* 锁存的最近有效心率（bpm） */
static uint64_t g_last_beat_ms = 0;    /* 最近一次检测到心跳的时刻（ms） */

/* ================================================================
 * 驱动层
 * ================================================================ */

/**
 * @brief 初始化 MAX30102
 *
 * 初始化序列（按数据手册）：
 *   1. 验证 PART_ID（应为 0x15）
 *   2. 软件复位（MODE_CFG bit6=1），等待清除
 *   3. 复位 FIFO 读写指针
 *   4. 配置 FIFO（无平均、无循环覆盖）
 *   5. 配置采样参数（100Hz、18bit ADC、411μs 脉宽）
 *   6. 配置 LED 电流（红光/红外各 ~6.2mA，0x1F * 0.2mA）
 *   7. 进入 SpO2 模式（红光+红外交替工作）
 *
 * @param fd        已打开的 I2C 文件描述符
 * @param dev_addr  MAX30102 的 7 位 I2C 地址（0x57）
 * @return          0 成功，-1 失败
 */
int max30102_init(int fd, uint8_t dev_addr)
{
    uint8_t part_id = 0;
    int ret;

    /* 1. 验证 PART_ID */
    ret = i2c_read_reg(fd, dev_addr, REG_PART_ID, &part_id, 1);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: 读取 PART_ID 失败\n");
        return -1;
    }
    if (part_id != 0x15) {
        fprintf(stderr, "max30102_init: PART_ID = 0x%02X，期望 0x15\n", part_id);
        return -1;
    }
    printf("[MAX30102] PART_ID = 0x%02X ✓\n", part_id);

    /* 2. 软件复位 */
    /*
     * MODE_CFG(0x09) bit6 RESET = 1。
     * 复位后该位自动清 0，所有寄存器恢复默认值。
     */
    ret = i2c_write_byte(fd, dev_addr, REG_MODE_CFG, 0x40);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: 软件复位失败\n");
        return -1;
    }
    usleep(100000);  /* 手册要求复位后等待至少 100μs，这里 100ms 确保安全 */

    /* 3. 复位 FIFO 指针 */
    /*
     * 写 FIFO_WR_PTR 和 FIFO_RD_PTR 为 0 不会精确复位内部计数器，
     * 正确的做法是连续读 FIFO_DATA 直到读空。但上电后 FIFO 为空，
     * 写指针寄存器足以完成初始化。
     */
    ret  = i2c_write_byte(fd, dev_addr, REG_FIFO_WR_PTR, 0x00);
    ret |= i2c_write_byte(fd, dev_addr, REG_FIFO_RD_PTR, 0x00);
    ret |= i2c_write_byte(fd, dev_addr, REG_FIFO_OVF_CNT, 0x00);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: FIFO 指针复位失败\n");
        return -1;
    }

    /* 4. FIFO 配置 */
    /*
     * FIFO_CFG(0x08)：
     *   bit[6:5] = 00 → SMP_AVE = 1（不平均，每个 sample 独立存储）
     *   bit[4]   = 0  → FIFO_ROLLOVER_EN = 0（FIFO 满后停止，不覆盖旧数据）
     *   bit[3:0] = 0000 → FIFO_A_FULL = 空（轮询模式用不到）
     *
     * 轮询模式写 0x00，中断模式写 0x0F（FIFO_A_FULL=15，剩 17 空位时触发中断）。
     */
    ret = i2c_write_byte(fd, dev_addr, REG_FIFO_CFG, 0x0F);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: FIFO 配置失败\n");
        return -1;
    }

    /* 5. SpO2 配置 */
    /*
     * SPO2_CFG(0x0A) = 0x07 → 0b00000111：
     *   bit[6:5] = 00 → ADC_RGE = 2048nA（满量程，LSB = 7.81pA）
     *   bit[4:2] = 001 → SPO2_SR = 100 samples/s
     *   bit[1:0] = 11  → LED_PW = 411μs，ADC 分辨率 = 18bit
     *
     * 100Hz 采样 + 411μs 脉宽：两个 LED 交替各占 411μs，
     * 一个完整 cycle = 2 × 411μs + 裕量 ≈ 1ms，刚好 1000 cycles/s。
     * 实际只用了 100 cycles/s，有充足的裕量。
     */
    ret = i2c_write_byte(fd, dev_addr, REG_SPO2_CFG, 0x07);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: SpO2 配置失败\n");
        return -1;
    }

    /* 6. LED 电流 */
    /*
     * LED 电流 ≈ 寄存器值 × 0.2mA（典型值）。
     * 0x1F = 31 → 31 × 0.2mA ≈ 6.2mA。
     * 初期使用低电流测试，避免烧毁 LED。实际使用中根据信号强度调整。
     *
     * LED1 = 红光（RED，660nm），LED2 = 红外（IR，880nm）。
     */
    ret  = i2c_write_byte(fd, dev_addr, REG_LED1_PA, 0x1F);
    ret |= i2c_write_byte(fd, dev_addr, REG_LED2_PA, 0x1F);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: LED 电流配置失败\n");
        return -1;
    }

    /* 7. 进入 SpO2 模式 */
    /*
     * MODE_CFG(0x09) = 0x03 → 0b00000011：
     *   bit7 SHDN = 0  → 正常工作（非关机）
     *   bit6 RESET = 0
     *   bit[2:0] = 011 → SpO2 模式（红光+红外交替采样）
     */
    ret = i2c_write_byte(fd, dev_addr, REG_MODE_CFG, 0x03);
    if (ret < 0) {
        fprintf(stderr, "max30102_init: 进入 SpO2 模式失败\n");
        return -1;
    }

    /* 8. 使能 FIFO_A_FULL 中断 */
    /*
     * INTR_ENABLE_1(0x02)：
     *   bit7 A_FULL_EN   = 1 → FIFO 达到 A_FULL 阈值时 INT 引脚拉低
     *   bit6 PPG_RDY_EN  = 1 → 每个 sample 就绪时触发（100Hz，过于频繁）
     *   bit5 ALC_OVF_EN  = 1 → 环境光消除溢出时触发
     *
     * 这里同时使能三个中断。实际使用中 A_FULL（FIFO≥15 个 sample）
     * 是主要的唤醒信号，约每 150ms 触发一次。
     *
     * 如果发现中断过于频繁（PPG_RDY 每 10ms 触发），可改为只使能 bit7：
     *   i2c_write_byte(fd, dev_addr, REG_INTR_ENABLE_1, 0x80);
     */
    ret = i2c_write_byte(fd, dev_addr, REG_INTR_ENABLE_1,(1 << 7));
    if (ret < 0) {
        fprintf(stderr, "max30102_init: 中断使能配置失败\n");
        return -1;
    }

    printf("[MAX30102] 初始化完成：100Hz / 18bit / LED≈6.2mA\n");
    return 0;
}


/**
 * @brief 初始化 MAX30102 中断引脚（GPIO 输入，双边沿检测）
 *
 * 数据流：
 *   MAX30102 INT 引脚（开漏输出，内部拉高）
 *     → NPi GPIO3_IO27（输入，下降沿触发）
 *       → gpiod_line_event → event fd → poll()
 *
 * 使用 libgpiod v0.x API，与 key_libgpiod.c 相同。
 *
 * @return event fd（给 poll() 使用），失败返回 -1
 *
 * 调用方拿到返回值后做 poll：
 *   int max30102_fd = max30102_int_init();
 *   struct pollfd pfd = { .fd = max30102_fd, .events = POLLIN };
 *   poll(&pfd, 1, -1);
 *   max30102_int_read_event();  // 清除事件
 */
int max30102_int_init(void)
{
    int event_fd;

    /* 1. 打开 GPIO 芯片（文件级静态变量，跨函数保持） */
    int_chip = gpiod_chip_open(CHIP_PATH);
    if (!int_chip) {
        perror("max30102_int_init: gpiod_chip_open");
        return -1;
    }

    /* 2. 获取中断引脚 */
    int_line = gpiod_chip_get_line(int_chip, MAX30102_INT);
    if (!int_line) {
        perror("max30102_int_init: gpiod_chip_get_line");
        gpiod_chip_close(int_chip);
        int_chip = NULL;
        return -1;
    }

    /* 3. 请求双边沿事件检测（INT 下降沿 + 恢复时的上升沿） */
    /*
     * MAX30102 INT 引脚是开漏输出——只能主动拉低，不能主动拉高。
     * 高电平必须靠上拉电阻。但当前内核版本不支持 GPIO 内部上拉配置，
     * 外部也未接上拉电阻，所以 INT 引脚始终为低电平，中断不可用。
     *
     * 不影响功能：main.c 已加入超时兜底，poll 超时后仍会查询 FIFO
     * 并读取数据。中断留到以后硬件补齐上拉电阻再启用。
     */
    if (gpiod_line_request_both_edges_events(int_line, "max30102_int") < 0) {
        perror("max30102_int_init: gpiod_line_request_both_edges_events");
        gpiod_chip_close(int_chip);
        int_chip = NULL;
        int_line = NULL;
        return -1;
    }

    /* 4. 获取 event fd —— 这个 fd 就是 poll 要等的对象 */
    event_fd = gpiod_line_event_get_fd(int_line);
    if (event_fd < 0) {
        perror("max30102_int_init: gpiod_line_event_get_fd");
        gpiod_line_release(int_line);
        gpiod_chip_close(int_chip);
        int_chip = NULL;
        int_line = NULL;
        return -1;
    }

    printf("[MAX30102] 中断引脚初始化完成：%s line %d (event_fd=%d)\n",
           CHIP_PATH, MAX30102_INT, event_fd);
    return event_fd;
}

/**
 * @brief 读取并清除 GPIO 中断事件（poll 返回后调用）
 *
 * 必须在 poll 返回后调用，否则 GPIO 控制器会持续认为有未处理事件，
 * 下次 poll 立即返回，造成"busy loop"。
 *
 * @return 0 成功，-1 失败（中断引脚未初始化或读取出错）
 */
int max30102_int_read_event(void)
{
    struct gpiod_line_event event;

    if (!int_line) {
        fprintf(stderr, "max30102_int_read_event: 中断引脚未初始化\n");
        return -1;
    }

    if (gpiod_line_event_read(int_line, &event) < 0) {
        perror("max30102_int_read_event");
        return -1;
    }

    return 0;
}

/**
 * @brief 释放 MAX30102 中断引脚资源
 *
 * 应在程序退出前调用。先释放 line 再关闭 chip，顺序不能反。
 */
void max30102_int_cleanup(void)
{
    if (int_line) {
        gpiod_line_release(int_line);
        int_line = NULL;
    }
    if (int_chip) {
        gpiod_chip_close(int_chip);
        int_chip = NULL;
    }
}


/**
 * @brief 查询 FIFO 中可读的 sample 数量
 *
 * FIFO 是 32 深度环形缓冲。写指针指向下一个写入位置，
 * 读指针指向下一个读取位置。
 *
 *   可用 sample 数 = (WR_PTR - RD_PTR) & 0x1F
 *
 * 返回值 0 表示 FIFO 为空。注意：如果恰好满了（32 个），
 * 差值也是 0，此时需结合 OVF_CNT 寄存器判断溢出——
 * 但在 100Hz 轮询场景下一般不会精确填满 32，先简化处理。
 *
 * @return 可用 sample 数（0~32），失败返回 -1
 */
int max30102_get_fifo_count(int fd, uint8_t dev_addr)
{
    uint8_t wr_ptr, rd_ptr;

    if (i2c_read_reg(fd, dev_addr, REG_FIFO_WR_PTR, &wr_ptr, 1) < 0) {
        fprintf(stderr, "max30102_get_fifo_count: 读 WR_PTR 失败\n");
        return -1;
    }
    if (i2c_read_reg(fd, dev_addr, REG_FIFO_RD_PTR, &rd_ptr, 1) < 0) {
        fprintf(stderr, "max30102_get_fifo_count: 读 RD_PTR 失败\n");
        return -1;
    }

    return (wr_ptr - rd_ptr) & 0x1F;
}

/**
 * @brief 从 FIFO 读取一个 sample（6 字节 → PPG_RawData_t）
 *
 * 每次读取 6 字节（3 字节红光 + 3 字节红外），
 * FIFO_DATA 寄存器支持自动递增——连续读 6 次自动拿到
 * 完整的 6 字节。读完后 MAX30102 内部读指针自动 +1。
 *
 * 注意：调用前应先用 max30102_get_fifo_count() 确认有数据可读，
 *       在 FIFO 为空时调用本函数会读到不确定的值。
 *
 * @return 0 成功，-1 失败
 */
int max30102_read_fifo(int fd, uint8_t dev_addr, PPG_RawData_t *raw)
{
    if (raw == NULL)
        return -1;

    uint8_t buf[6];

    if (i2c_read_reg(fd, dev_addr, REG_FIFO_DATA, buf, 6) < 0) {
        fprintf(stderr, "max30102_read_fifo: 读 FIFO_DATA 失败\n");
        return -1;
    }

    /*
     * 数据拼接（大端序）：
     *   buf[0] = RED[17:10]  ← 高字节
     *   buf[1] = RED[9:2]
     *   buf[2] = RED[1:0]    ← 低字节（只有 bit[7:6] 有效）
     *   buf[3] = IR[17:10]
     *   buf[4] = IR[9:2]
     *   buf[5] = IR[1:0]
     *
     * 最后用 ADC_MASK_18BIT 截断，确保高位噪声归零。
     */
    raw->red  = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
    raw->red &= ADC_MASK_18BIT;

    raw->ir   = ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | buf[5];
    raw->ir  &= ADC_MASK_18BIT;

    return 0;
}

/* ================================================================
 * PPG 信号处理层
 * ================================================================ */

/**
 * @brief 计算 DC 基线和 AC 分量
 *
 * DC 基线 = 滑动窗口内所有样本的平均值（反映组织/静脉的静态吸收）。
 * AC 分量 = 当前值 − DC 基线（反映动脉搏动引起的反射变化）。
 *
 * 窗口填充策略：
 *   窗口未满时（前 128 个 sample），只用已填充的样本数做平均，
 *   避免零值拉低 DC——否则 AC 会异常大，导致 R 比值和 SpO2 离谱。
 *   窗口填满后，用全部 128 个样本做平均。
 */
static void ppg_get_acdc(uint32_t red_raw, uint32_t ir_raw, PPG_ACDC_t *acdc)
{
    /* 环形写入 */
    red_buf[buf_idx] = (float)red_raw;
    ir_buf[buf_idx]  = (float)ir_raw;
    buf_idx = (buf_idx + 1) % PPG_WINDOW_SIZE;

    /* 窗口填充计数 */
    if (buf_fill_count < PPG_WINDOW_SIZE)
        buf_fill_count++;

    /*
     * 全窗口求和 + 找峰谷值：
     *   DC 基线 = 均值（组织/静脉静态吸收）
     *   峰峰值   = max - min（动脉搏动引起的完整摆幅）
     *
     * 为什么血氧要用峰峰值而不是瞬时值（关键修复）：
     *   脉搏 AC 是正弦波动，瞬时值在波峰/波谷/过零点之间来回扫，
     *   用瞬时 AC 算 R 比值会得到 0/0 或剧烈跳变的值，导致 SpO2
     *   绝大部分时刻落在无效区间（<80 或 >100 被裁剪）。
     *   峰峰值是窗口内的完整摆幅，稳定，R 比值和 SpO2 才可信。
     */
    float red_sum = 0.0f, ir_sum = 0.0f;
    float red_max = red_buf[0], ir_max = ir_buf[0];
    float red_min = red_buf[0], ir_min = ir_buf[0];
    for (int i = 0; i < PPG_WINDOW_SIZE; i++) {
        red_sum += red_buf[i];
        ir_sum  += ir_buf[i];
        if (red_buf[i] > red_max) red_max = red_buf[i];
        if (red_buf[i] < red_min) red_min = red_buf[i];
        if (ir_buf[i]  > ir_max)  ir_max  = ir_buf[i];
        if (ir_buf[i]  < ir_min)  ir_min  = ir_buf[i];
    }

    /* DC = 总和 / 有效样本数 */
    acdc->red_dc = red_sum / (float)buf_fill_count;
    acdc->ir_dc  = ir_sum  / (float)buf_fill_count;

    /* 瞬时 AC = 原始值 − DC 基线（心率峰值检测用） */
    acdc->red_ac = (float)red_raw - acdc->red_dc;
    acdc->ir_ac  = (float)ir_raw  - acdc->ir_dc;

    /* 峰峰值 AC = max - min（血氧 R 比值用，窗口内稳定） */
    acdc->red_ac_pp = red_max - red_min;
    acdc->ir_ac_pp  = ir_max  - ir_min;
}

/**
 * @brief 计算 R 比值（用于血氧计算）
 *
 * R = (AC_red / DC_red) / (AC_ir / DC_ir)
 *
 * 这是脉搏血氧法的核心参数。不同 SpO2 对应不同的 R 值，
 * 通过经验校准曲线 → SpO2。
 */
static float ppg_calc_r(const PPG_ACDC_t *acdc)
{
    /*
     * R = (AC_red / DC_red) / (AC_ir / DC_ir)
     *
     * AC 用峰峰值（红光峰的完整摆幅 / 红外峰的完整摆幅），不是瞬时值。
     * 峰峰值在窗口内稳定，R 比值才稳定；瞬时值会随波形相位剧烈跳变。
     */
    float red_ratio = acdc->red_ac_pp / acdc->red_dc;
    float ir_ratio  = acdc->ir_ac_pp  / acdc->ir_dc;

    /* 防除零：红外 AC 摆幅接近 0 说明没有有效脉搏，返回 0 让上层判无效 */
    if (ir_ratio < 1e-6f)
        return 0.0f;

    return red_ratio / ir_ratio;
}

/**
 * @brief R 比值 → 血氧饱和度（经验公式）
 *
 * 公式来源：MAX30102 社区常用的二次多项式拟合。
 * SpO2 = -45.06·R² + 30.354·R + 94.845
 *
 * 注意：这是经验公式，非医疗器械级校准。实际精度取决于
 * 硬件布局、LED 波长、个体差异等。仅供学习和原型验证。
 */
static uint8_t ppg_r_to_spo2(float R)
{
    /* 经验公式仅在 R ∈ [0.2, 2.0] 内有效，超范围时裁剪 */
    if (R < 0.2f) R = 0.2f;
    if (R > 2.0f) R = 2.0f;

    float spo2 = -45.06f * R * R + 30.354f * R + 94.845f;

    if (spo2 > 100.0f) spo2 = 100.0f;
    if (spo2 < 0.0f)   spo2 = 0.0f;

    return (uint8_t)spo2;
}

/**
 * @brief 红外 AC 信号 → 瞬时心率（自适应阈值峰值检测法）
 *
 * 算法：
 *   1. 追踪近期 ir_ac 正峰值 ir_ac_peak，动态阈值 = 峰值的一半
 *   2. 当 ir_ac 从下方穿过动态阈值时，检测到峰值
 *   3. 记录当前时间，与上次峰值时间差 = IBI（搏动间隔）
 *   4. HR = 60000 / IBI_ms
 *   5. 迟滞：峰值标记保持到 ir_ac 降至阈值一半以下才复位，
 *      避免单个波峰被重复计数
 *
 * 为什么不用固定阈值？
 *   不同人、不同手指贴合压力、不同 LED 电流下，脉搏 AC 幅值差别可达
 *   数倍。固定阈值（如原来的 800）要么对弱信号太高（永远检测不到），
 *   要么对强信号太低（噪声误触发）。用"峰值的 50%"做动态阈值，阈值
 *   会跟随实际信号强度自动伸缩。
 *
 * 峰值衰减：每个 sample 乘 0.995，100Hz 下约 2s 衰减到 1/3。
 *   这样手指拿开后阈值会慢慢回落，重新贴合后能重新锁定；同时
 *   也避免某一次异常大尖峰把阈值永久抬高。
 *
 * @param ir_ac  红外 AC 分量
 * @param ir_dc  红外 DC 基线（用于 DC 漂移瞬态保护）
 * @param sys_ms 当前系统时间（毫秒），用于计算 IBI
 * @return       瞬时心率（bpm），未检测到峰值时返回 0
 */
static uint8_t ppg_calc_hr(float ir_ac, float ir_dc, uint64_t sys_ms)
{
    uint8_t hr = 0;

    /*
     * DC 漂移瞬态保护（关键）：
     *
     * 正常脉搏 AC 只有 DC 基线的 1%~2%（本板实测 ir AC 2000~3000 / DC 20万）。
     * 但手指贴合、移动、改变压力时，DC 基线会阶跃/漂移几万到十几万，而
     * DC 是 128 样本滑动平均，跟不上这种突变，导致 ir_ac 瞬时爆炸到 DC 的
     * 百分之几十甚至更大。这种"假 AC"不是脉搏，若让它更新 ir_ac_peak，
     * 会把自适应阈值抬到远高于真实脉搏的高度，之后即使手指稳定也检测
     * 不到峰（0.995 衰减要 6 秒以上才回得去）。
     *
     * 两者幅度差 10 倍以上，故以 DC 的 10% 为界：
     *   |ir_ac| <= 10% DC → 正常脉搏，参与峰值检测；
     *   |ir_ac| >  10% DC → DC 漂移瞬态，跳过本次检测，只让阈值缓慢回落，
     *                       不污染 ir_ac_peak。
     * 10% 是保守值：强脉搏也难超过 DC 的 5%，漂移瞬态则远超 20%。
     */
    if (ir_dc > 0.0f && fabsf(ir_ac) > ir_dc * 0.10f) {
        ir_ac_peak *= 0.995f;   /* 漂移期间也让阈值回落，不冻结 */
        return 0;
    }

    /* 追踪正峰值：只升不降，靠下面的衰减因子缓慢回落 */
    if (ir_ac > ir_ac_peak)
        ir_ac_peak = ir_ac;
    ir_ac_peak *= 0.995f;

    /* 动态阈值 = 峰值的 50%，下限 100 防止无信号时噪声误触发 */
    float thresh = ir_ac_peak * 0.5f;
    if (thresh < PPG_PEAK_THRESH_MIN)
        thresh = (float)PPG_PEAK_THRESH_MIN;

    /* 上升沿穿过阈值 → 检测到峰值 */
    if (ir_ac > thresh && peak_flag == 0) {
        peak_flag = 1;

        uint64_t delta = sys_ms - last_peak_ms;
        last_peak_ms = sys_ms;

        /*
         * IBI 有效范围：300ms ~ 2000ms，对应 30~200 bpm。
         * 超出这个范围的视为噪声或丢峰。
         */
        if (delta >= 300 && delta <= 2000) {
            hr = (uint8_t)(60000.0f / (float)delta);
        }
    }

    /* 迟滞：降至阈值一半以下才允许下次检测 */
    if (ir_ac < thresh * 0.5f) {
        peak_flag = 0;
    }

    return hr;
}

/**
 * @brief PPG 原始数据 → 心率/血氧（统一处理入口）
 *
 * 处理流程：
 *   RAW → AC/DC 分离 → R 比值 → SpO2
 *                     → 峰值检测 → HR
 *                     → 有效性判断
 *
 * @param raw    一个 sample 的原始数据（红光+红外）
 * @param sys_ms 当前系统时间戳（毫秒），心率计算需要
 * @return       PPG_Result_t，含心率、血氧、有效标志
 *
 * 注意：窗口未填满时 data_valid 恒为 false，
 *       因为 DC 基线未稳定，计算结果不可信。
 */
PPG_Result_t ppg_process(PPG_RawData_t raw, uint64_t sys_ms)
{
    PPG_Result_t res = {0};
    PPG_ACDC_t acdc;

    ppg_get_acdc(raw.red, raw.ir, &acdc);

    /* 窗口未满 → DC 未稳定 → 不计算 PPG */
    if (buf_fill_count < PPG_WINDOW_SIZE) {
        res.data_valid = false;
        return res;
    }

    /* 血氧计算：用峰峰值 AC（窗口内稳定），不再用瞬时 AC */
    float R = ppg_calc_r(&acdc);
    res.spo2 = ppg_r_to_spo2(R);

    /* 心率计算：瞬时 IR AC 峰值检测 */
    uint8_t new_hr = ppg_calc_hr(acdc.ir_ac, acdc.ir_dc, sys_ms);
    if (new_hr != 0) {
        /* 检测到一次有效心跳：锁存心率 + 记录时刻 */
        g_heart_rate   = new_hr;
        g_last_beat_ms = sys_ms;
    }

    /*
     * 心率锁存（关键修复）：
     * 峰值检测只在"上升沿穿过阈值"的那一个 sample 返回心率，其余
     * 时刻返回 0。若直接用返回值参与有效性判断，两个心跳之间的
     * 几十个 sample 都会因 heart_rate=0 被判无效——这正是之前
     * "绝大多数时间无效"的另一半原因。
     * 所以锁存最近一次有效心率，直到超时才失效：
     *   手指稳定 → 每个心跳周期刷新一次，期间保持旧值；
     *   手指拿开 → 2 秒内没有新心跳，心率归 0，数据判无效。
     */
    if (sys_ms - g_last_beat_ms > 2000)
        g_heart_rate = 0;

    res.heart_rate = g_heart_rate;

    /* 有效性判断 */
    if (res.spo2 >= 80 && res.heart_rate >= 30)
        res.data_valid = true;
    else
        res.data_valid = false;

    return res;
}

/**
 * @brief 查询 DC 窗口已填充的样本数
 *
 * 返回值 < PPG_WINDOW_SIZE 时，DC 基线尚未稳定，
 * PPG 计算结果不可信。调用者可据此显示"预热中"。
 */
int ppg_get_fill_count(void)
{
    return (int)buf_fill_count;
}
