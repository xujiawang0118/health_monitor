# i2c-0 总线瘫痪与 GPIO bitbang 恢复 —— 复盘

> 平台：野火 EBF6ull Pro（NXP i.MX6ULL，Cortex-A7）
> 内核：4.19.35-imx6（野火 ebf_linux_kernel）
> 涉及项目：health_monitor
> 时间：2026-09 中下旬

---

## 0. 一句话版本

i2c-0 上某个从机把 SDA 拉死不放，i.MX6ULL 的 I2C 控制器把这判定成"仲裁丢失"（`I2SR_IAL`），
之后每一次传输都在第一步就失败，总线**永久报废**。

i2c-imx 驱动**自带**总线恢复功能（把 SCL/SDA 切成 GPIO，手动敲时钟把从机敲醒），
但设备树没给它配 `pinctrl "gpio"` 状态和 `scl-gpios`/`sda-gpios`，
所以这套恢复逻辑**从未被激活**。补上这两样之后，恢复通路才真正接通。

---

## 1. 总线拓扑

`/dev/i2c-0` 这一个总线上挂着三方从机：

| 从机 | 地址 | 谁在访问 |
|---|---|---|
| MPU6050（六轴） | 0x68 | 内核，`compatible = "kosaki,mpu6050"` |
| **GT1151（电容触摸屏）** | **0x14** | **内核，`compatible = "goodix,gt1151"`** |
| MAX30102（心率血氧） | — | **纯用户态**，经 `/dev/i2c-0` + ioctl |

> **命名陷阱（见坑 1）**：设备树里这个控制器叫 `&i2c1`（地址 `0x021a0000`），
> 但在 Linux 里它枚举成 **`i2c-0`**，对应 `/dev/i2c-0`。
> 排查时看到 `i2c1` 千万别以为"那是另一个总线"。

GT1151 是重点嫌疑：它是**内核驱动在跑**的从机，采样率高、时序密集，
而 MAX30102 和 MPU6050 相对"听话"。触摸控制器在复位/半途被打断时最容易
让内部移位寄存器失去同步，把 SDA 拉死。

---

## 2. 现象

- 程序跑几分钟后，所有 I2C 访问开始报错，`errno = 11 (EAGAIN)`
- **再也恢复不了**：不是偶发失败，是从那之后每次访问都失败
- 只有复位板子才能救回来
- 跑 1 小时那次没有复现（说明它有触发条件，不是必然发生）

---

## 3. 排查过程

### 3.1 第一步：errno 11 到底是谁返回的

i.MX6ULL 的 I2C 控制器状态寄存器 `I2SR` 里有一位 `IAL`（Arbitration Lost，bit 4）。

翻 `drivers/i2c/busses/i2c-imx.c:425` 的 `i2c_imx_bus_busy()`：

```c
static int i2c_imx_bus_busy(struct imx_i2c_struct *i2c_imx, int for_busy)
{
	unsigned long orig_jiffies = jiffies;
	unsigned int temp;

	while (1) {
		temp = imx_i2c_read_reg(i2c_imx, IMX_I2C_I2SR);

		/* check for arbitration lost */
		if (temp & I2SR_IAL) {
			temp &= ~I2SR_IAL;
			imx_i2c_write_reg(temp, i2c_imx, IMX_I2C_I2SR);
			return -EAGAIN;              /* ← 唯一的 EAGAIN 出处 */
		}

		if (for_busy && (temp & I2SR_IBB)) { i2c_imx->stopped = 0; break; }
		if (!for_busy && !(temp & I2SR_IBB)) { i2c_imx->stopped = 1; break; }

		if (time_after(jiffies, orig_jiffies + msecs_to_jiffies(500))) {
			dev_dbg(&i2c_imx->adapter.dev,
				"<%s> I2C bus is busy\n", __func__);
			return -ETIMEDOUT;           /* ← 另一种死法：总线一直忙 */
		}
		schedule();
	}
	return 0;
}
```

**这个函数里 `-EAGAIN` 只有一个出处：IAL。**
整个 `i2c-imx.c` 里 `EAGAIN` 也只出现在这一行（其余全是 `-ETIMEDOUT`）。

```bash
grep -n "EAGAIN\|ETIMEDOUT" drivers/i2c/busses/i2c-imx.c
# 439: return -EAGAIN;      ← 只有这一处
# 453, 467, 665, 676, 720, 731: return -ETIMEDOUT;
```

所以 **`errno = 11` 精确对应 `I2SR_IAL`，没有任何歧义**。
（`EAGAIN` = 11，`ETIMEDOUT` = 110 —— 如果是 110 就是另一码事：总线一直忙。）

这一步的价值：**把"I2C 读失败"这个笼统现象，收窄到一个确定的硬件位**。
后面所有分析都建立在这上面。

### 3.2 第二步：IAL 到底意味着什么

这是最容易被误解的一步。

I2C 是**开漏 + 线与**总线：任何设备**只能把线拉低，不能主动拉高**；
线要变高，靠的是上拉电阻。所以"读回低电平"和"有人驱动低电平"是**同一件事**，
谁在拉低是分不出来的。

主机的正常流程是：发送时自己驱动 SDA，接收 ACK 时**释放 SDA（松手，让上拉电阻拉高）**，
然后读回来看从机有没有把它拉低。

**如果主机松手了，却读回低电平 —— 从控制器的视角看，就是"别人在驱动 SDA"，
于是置 `IAL`。**

在一个只有单主机的总线上，"别人"只可能是**卡住的从机**：
它内部移位寄存器失去了和主机的同步，认为字节还没传完，于是死死拉住 SDA 不放。

> **面试点**：`IAL` 这个名字有误导性 —— 它听起来像"两个主机抢总线"。
> 在单主机系统里，它是**从机卡死**的典型征兆。判断依据永远是物理层：
> 松手了还是低，就有人拉着。

### 3.3 第三步：为什么"永久"报废

无恢复逻辑时，`IAL` 那一位被驱动清掉了（`imx_i2c_write_reg(temp, ..., IMX_I2C_I2SR)`），
但**物理层的僵局没解开** —— 从机还拉着 SDA。

下一次传输，`i2c_imx_start()`（:552）第一件事就是
`i2c_imx_bus_busy(i2c_imx, 1)`（:575），等总线变忙。
它会读到 IAL 吗？不一定。但紧接着发 START 条件时，
主机把 SDA 释放为高、却发现是低的 —— **又一次 IAL**。

于是每一次传输都在第一步倒掉，从应用层看就是"总线永久死了"。
**清标志位 ≠ 解决问题**，问题在物理层。

### 3.4 第四步：驱动明明有恢复功能，为什么没生效

翻到 `i2c_imx_init_recovery_info()`（:1038）：

```c
static int i2c_imx_init_recovery_info(struct imx_i2c_struct *i2c_imx,
		struct platform_device *pdev)
{
	struct i2c_bus_recovery_info *rinfo = &i2c_imx->rinfo;

	i2c_imx->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (!i2c_imx->pinctrl || IS_ERR(i2c_imx->pinctrl)) {
		dev_info(&pdev->dev, "can't get pinctrl, bus recovery not supported\n");
		return PTR_ERR(i2c_imx->pinctrl);
	}

	i2c_imx->pinctrl_pins_default = pinctrl_lookup_state(i2c_imx->pinctrl,
			PINCTRL_STATE_DEFAULT);
	i2c_imx->pinctrl_pins_gpio = pinctrl_lookup_state(i2c_imx->pinctrl,
			"gpio");                                   /* ← 字面必须是 "gpio" */
	rinfo->sda_gpiod = devm_gpiod_get(&pdev->dev, "sda", GPIOD_IN);
	rinfo->scl_gpiod = devm_gpiod_get(&pdev->dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);
	                                                   /* ← 对应 sda-gpios / scl-gpios */

	if (PTR_ERR(rinfo->sda_gpiod) == -EPROBE_DEFER ||
	    PTR_ERR(rinfo->scl_gpiod) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(rinfo->sda_gpiod) ||
		   IS_ERR(rinfo->scl_gpiod) ||
		   IS_ERR(i2c_imx->pinctrl_pins_default) ||
		   IS_ERR(i2c_imx->pinctrl_pins_gpio)) {
		dev_dbg(&pdev->dev, "recovery information incomplete\n");   /* ← 静默失败 */
		return 0;
	}

	dev_dbg(&pdev->dev, "using scl%s for recovery\n",
		rinfo->sda_gpiod ? ",sda" : "");          /* ← 打印 "using scl,sda for recovery" */

	rinfo->prepare_recovery = i2c_imx_prepare_recovery;
	rinfo->unprepare_recovery = i2c_imx_unprepare_recovery;
	rinfo->recover_bus = i2c_generic_scl_recovery;
	i2c_imx->adapter.bus_recovery_info = rinfo;    /* ← 就这一行决定恢复功能有没有 */

	return 0;
}
```

**关键在最后一行。** `adapter.bus_recovery_info` 是 NULL 还是指针，
决定了上层会不会去调恢复函数。

原始的野火设备树里，`&i2c1` 只有：

```dts
&i2c1 {
	pinctrl-names = "default";
	pinctrl-0 = <&pinctrl_i2c1>;
	...
};
```

- `pinctrl_lookup_state(..., "gpio")` → 名字不在 `pinctrl-names` 里 → 返回 `ERR_PTR(-ENODEV)`
- 于是命中 `"recovery information incomplete"` 分支 → **`return 0`（成功返回！）**
- `bus_recovery_info` 保持 NULL

**这是最阴的一步：函数返回 0（成功），驱动 probe 正常通过，什么都没坏。
只是恢复功能悄悄没了。** 而且那行提示是 `dev_dbg`，默认关，连个日志都没有。

### 3.5 三个必要条件

恢复功能要生效，必须同时满足：

| # | 条件 | 对应设备树 |
|---|---|---|
| 1 | 有名为 `"gpio"` 的 pinctrl 状态 | `pinctrl-names = "default", "gpio";` + `pinctrl-1` |
| 2 | 该状态把 SCL/SDA 两个 pad 切成 GPIO 功能 | `pinctrl_i2c1_gpio` 组里 `...__GPIO1_IO28/29` |
| 3 | 有 `sda-gpios` / `scl-gpios` 属性 | `sda-gpios = <&gpio1 29 ...>;` |

**少任何一条，整条恢复链路都是死的，而且没有任何报错。**

---

## 4. 补丁

文件：`arch/arm/boot/dts/imx6ull-mmc-npi.dts`（内核源码树里）

### 4.1 新增 GPIO 状态（:352）

```dts
/*
 * 总线恢复用的 GPIO 状态：把 I2C1 的 SCL/SDA 两脚切成普通 GPIO。
 * 引脚编号必须和 pinctrl_i2c1 是同一对：
 *   UART4_TX_DATA -> GPIO1_IO28 (SCL)
 *   UART4_RX_DATA -> GPIO1_IO29 (SDA)
 * pad 配置值直接复用 pinctrl_i2c1 的值，让切换状态时电气特性不变，
 * 只有 mux mode 变了（参考 imx6ull-colibri.dtsi 的同样写法）。
 */
pinctrl_i2c1_gpio: i2c1-gpio-grp {
	fsl,pins = <
		MX6UL_PAD_UART4_TX_DATA__GPIO1_IO28 0x4001b8b0
		MX6UL_PAD_UART4_RX_DATA__GPIO1_IO29 0x4001b8b0
	>;
};
```

**为什么 pad 配置值要原样复用 `0x4001b8b0`？**

这个值的字段是：`HYS=1, PUS=100K上拉, PUE=1, PKE=1, ODE=1, SPEED=10, DSE=6, SRE=0`。

重点是 **`ODE = 1`（Open Drain Enable，bit 11）** —— pad 的开漏模式**和 mux mode 无关**，
即使切成 GPIO 依然保持开漏。这正是 I2C 需要的电气特性：
只能拉低、不能推高，否则会和外设对推烧引脚。

切状态前只有 mux mode 变了，其余电气配置一字不动 —— 这是有意为之。

### 4.2 改造 `&i2c1` 节点（:441）

```dts
&i2c1 {
	clock-frequency = <100000>;
	pinctrl-names = "default", "gpio";        /* ← 名字必须字面是 "gpio" */
	pinctrl-0 = <&pinctrl_i2c1>;
	pinctrl-1 = <&pinctrl_i2c1_gpio>;

	/*
	 * 总线恢复用：i2c-imx 会 devm_gpiod_get(dev, "sda"/"scl")，
	 * 属性名必须字面是 sda-gpios / scl-gpios。
	 * GPIO1_IO29 = UART4_RX_DATA = I2C1_SDA
	 * GPIO1_IO28 = UART4_TX_DATA = I2C1_SCL
	 * 必须用 ACTIVE_HIGH：i2c_generic_scl_recovery 靠 "get_scl() 非 0
	 * 表示物理高" 来判断，gpiod_get/set_value 会按 ACTIVE_LOW 取反。
	 */
	sda-gpios = <&gpio1 29 GPIO_ACTIVE_HIGH>;
	scl-gpios = <&gpio1 28 GPIO_ACTIVE_HIGH>;

	status = "okay";

	mpu6050@68 {
		compatible = "kosaki,mpu6050";   /* 自己的前缀，避开官方驱动 */
		reg = <0x68>;
		interrupt-parent = <&gpio5>;
		interrupts = <0 1>;              /* GPIO5_0，1 = IRQ_TYPE_EDGE_RISING */
	};
};
```

---

## 5. 恢复是怎么跑的（完整调用链）

从用户态一行 `ioctl(fd, I2C_RDWR, &msgset)` 开始：

```
用户态 ioctl(I2C_RDWR)
  └─ i2c-dev → i2c_transfer → __i2c_transfer
      └─ i2c_imx_xfer()                                  i2c-imx.c:916
          ├─ i2c_imx_start()                             :552
          │   └─ i2c_imx_bus_busy(i2c_imx, 1)            :575
          │       └─ 读到 I2SR_IAL → 清位 → return -EAGAIN   :439
          │
          ├─ result != 0 且 adapter.bus_recovery_info 非空   :940
          │   ├─ i2c_recover_bus(&adapter)               i2c-core-base.c:245
          │   │   └─ i2c_generic_scl_recovery()          :185
          │   │       ├─ prepare_recovery()              i2c-imx.c:1013
          │   │       │   └─ pinctrl_select_state("gpio")   ← SCL/SDA 变 GPIO
          │   │       ├─ 输出 9 个 SCL 时钟（~100kHz）
          │   │       │   每拍上升沿后 i2c_generic_bus_free() 读 SDA
          │   │       │   SDA 一旦变高 → 提前 break（总线通了）
          │   │       └─ unprepare_recovery()            :1022
          │   │           └─ pinctrl_select_state("default") ← 切回 I2C 功能
          │   └─ 重试 i2c_imx_start() 一次                :942
          │
          └─ 成功 → 继续正常传输；仍失败 → -EAGAIN 冒到用户态
```

### 5.1 为什么是 9 个时钟

`i2c-core-base.c:182`：

```c
#define RECOVERY_NDELAY		5000    /* 半周期 5µs → 100kHz */
#define RECOVERY_CLK_CNT	9
```

I2C 一个字节是 **8 位数据 + 1 位 ACK = 9 个时钟**。
从机如果卡在某个字节中途，它内部还差几个位才能走完；
**最多再给它 9 拍，它一定能把当前字节走完**，进入能识别 STOP 的状态，然后释放 SDA。

这就是 I2C 规范（UM10204 §3.1.16 "Bus clear"）里写的方法，
i.MX6ULL 参考手册的 I2C 章节也引用了同一套做法。

### 5.2 SCL 卡低就放弃

```c
while (i++ < RECOVERY_CLK_CNT * 2) {
	if (scl) {
		/* SCL shouldn't be low here */
		if (!bri->get_scl(adap)) {
			dev_err(&adap->dev, "SCL is stuck low, exit recovery\n");
			ret = -EBUSY;
			break;
		}
	}
	...
```

SCL 被拉死说明是**更硬的故障**（短路、从机彻底锁死、上拉电阻掉了）。
这种敲时钟没用，直接 `-EBUSY` 退出，而且这一条是 `dev_err` —— **会打日志**。

### 5.3 一个容易看漏的细节：本驱动不做显式 STOP

`i2c-core-base.c:268`：

```c
	if (bri->scl_gpiod && bri->recover_bus == i2c_generic_scl_recovery) {
		bri->get_scl = get_scl_gpio_value;
		bri->set_scl = set_scl_gpio_value;
		if (bri->sda_gpiod) {
			bri->get_sda = get_sda_gpio_value;
			/* FIXME: add proper flag instead of '0' once available */
			if (gpiod_get_direction(bri->sda_gpiod) == 0)
				bri->set_sda = set_sda_gpio_value;
		}
	}
```

`set_sda` **只在 SDA 是输出方向时才绑定**（`gpiod_get_direction() == 0` 表示输出）。
而 i2c-imx 申请 SDA 时用的是 `GPIOD_IN`（:1053），方向是输入
—— 所以 `set_sda` 是 NULL。

后果：`i2c_generic_scl_recovery()` 里那两处
`if (bri->set_sda) bri->set_sda(adap, scl);` 全被跳过，
**代码注释里说的"顺便补一个 STOP 条件"在本平台上没有执行**。
实际只做两件事：敲 9 拍 SCL + 读 SDA 判断是否脱困。

这对 i2c-imx 是够的（从机释放 SDA 就够了，不缺那个 STOP），
但**读源码时不能只看注释，要看条件编译/运行时判断**。

---

## 6. 踩过的坑

### 坑 1：设备树 `i2c1` ≠ Linux `i2c1`

| | 名字 |
|---|---|
| 设备树节点 | `&i2c1`，地址 `0x021a0000` |
| Linux 枚举 | **`/dev/i2c-0`** |
| 内核 debugfs | `/sys/kernel/debug/…` |

i.MX6ULL 的命名是 I2C1/I2C2/I2C3/I2C4，Linux 从 0 开始编号，
**错位一个**。所以 `&i2c1` 上的改动，影响的是 `/dev/i2c-0`。

野火那个 overlay 叫 **`imx-fire-i2c1.dtbo`** —— 名字看起来像"另一个总线"，
实际上**就是你的 /dev/i2c-0**。这个误判耽误了不少时间。

### 坑 2：野火 overlay 静默覆盖（最坑的一个）

`arch/arm/boot/dts/overlays/imx-fire-i2c1-overlay.dts`：

```dts
fragment@0 {
    target = <&i2c1>;
    __overlay__ {
        clock-frequency = <100000>;
        pinctrl-names = "default";          /* ← 把 "default","gpio" 覆盖掉！ */
        pinctrl-0 = <&pinctrl_i2c1>;
        status = "okay";
        ...
    };
};
```

overlay 应用时，`__overlay__` 里的属性是**替换**语义，不是合并。
所以基础 dtb 里辛苦写好的 `pinctrl-names = "default", "gpio"`
会被覆盖成只剩 `"default"` → `pinctrl_lookup_state(..., "gpio")` 失败
→ 恢复功能消失。

**而 `scl-gpios`/`sda-gpios` 反而会保留**（overlay 没碰它们），
于是设备树里能看到 `scl-gpios` 属性、看起来"补丁打上了"，
但恢复功能其实是死的 —— **半真半假的状态最容易骗过人**。

处理：`/boot/uEnv.txt` 里把加载这个 overlay 的那行注释掉。

> 这个 overlay 本身也是多余的：基础 dts 里 `&i2c1` 已经声明了 `mpu6050@68`，
> 而且那个 overlay 的 `fragment@1` 还重复定义了 `pinctrl_i2c1: i2c1grp` 标签。
> （顺带一提，它的文件头注释写的是 `imx-fire-mpu6050-overlay.dts` —— 野火复制粘贴没改。）

**更普适的教训：查"设备树到底生效成什么样"，永远看运行时的
`/proc/device-tree/`，不要看源码。** 中间可能隔着 dtb、overlay、uEnv.txt 好几层。

### 坑 3：`GPIO_ACTIVE_HIGH` 不能省

```c
if (bri->get_scl(adap) == 0) → 报 "SCL is stuck low" 退出
```

`get_scl` 最终是 `gpiod_get_value()`，它会**按设备树里的 ACTIVE 标志取反**。
写成 `GPIO_ACTIVE_LOW` 的话：

- 物理高 → 读回 0 → 代码以为"SCL 卡低" → **立刻放弃恢复**
- `set_scl(1)` 实际输出物理低 → 时钟极性也是反的

而且失败信息是 `dev_err("SCL is stuck low")` ——
**它会误导你以为是硬件坏了，其实是软件配置反了**。这是本坑最毒的地方。

### 坑 4：失败路径全是 `dev_dbg` —— "没日志" ≠ "没生效"

- `"recovery information incomplete"` → `dev_dbg`
- `"using scl,sda for recovery"` → `dev_dbg`
- 恢复成功/失败 → `dev_dbg`
- 只有 `"SCL is stuck low"` 是 `dev_err`

**默认日志级别下，恢复功能成功也是静默、失败也是静默。**
所以"跑了一小时没瘫痪，也没看到日志"**不能**证明补丁生效 ——
同样可以解释成"这一小时总线压根没进过 IAL"。

要拿到证据必须开动态调试（`CONFIG_DYNAMIC_DEBUG`）：

```bash
echo -n "file i2c-imx.c +p" | sudo tee /sys/kernel/debug/dynamic_debug/control
sudo dmesg -C
echo 21a0000.i2c | sudo tee /sys/bus/platform/drivers/imx-i2c/unbind
echo 21a0000.i2c | sudo tee /sys/bus/platform/drivers/imx-i2c/bind
sudo dmesg | grep -iE "recovery|scl,sda"
```

期望看到 `using scl,sda for recovery`（证明 `bus_recovery_info` 挂上了）；
如果看到 `recovery information incomplete`，说明三个必要条件还差。

> **这条是通用的排查素养**：区分"代码路径没走到"和"代码路径走到了但没打印"。
> 中间隔着的就是日志级别。

### 坑 5：pinctrl 的引脚必须是同一对

`pinctrl_i2c1` 用的是 `MX6UL_PAD_UART4_TX_DATA`（SCL）和 `MX6UL_PAD_UART4_RX_DATA`（SDA）。
新的 GPIO 组必须是**同样两个 pad** 的 GPIO 功能版本，
而且 `scl-gpios`/`sda-gpios` 里的 GPIO 编号要**和它们一一对应**。

对错了会得到一个很迷惑的现象：恢复逻辑跑完了、日志也打了，
但敲的是另外两个脚 —— 总线当然没救回来。

推导路径：`UART4_TX_DATA` → `GPIO1_IO28`，
`UART4_RX_DATA` → `GPIO1_IO29`（查 `imx6ul-pinfunc.h` 里的宏定义）。
`&gpio1 28` 里的 `28` 是 **`GPIO1_IO28` 的编号**，不是 pad 编号 ——
这个换算错一次就得重来。

---

## 7. 验证方法与当前状态

### 已确认（设备树层面）

```bash
cat /proc/device-tree/soc/aips-bus@2100000/i2c@21a0000/pinctrl-names
# → defaultgpio     （两个字符串连着，说明 "default" 和 "gpio" 都在）
# → 只输出 default  = 被 overlay 覆盖了

ls /proc/device-tree/soc/aips-bus@2100000/i2c@21a0000/ | grep gpios
# → scl-gpios  sda-gpios
```

> **注意路径**：i2c 节点在 `soc/aips-bus@2100000/` 下，**不在 `soc/` 下**。
> `ls /proc/device-tree/soc/ | grep i2c` 返回空是**层级找错了**，不是补丁失败。

同时说明 overlay `imx-fire-i2c1.dtbo` **确认未被加载**（否则 `pinctrl-names` 只剩 `default`）。

### 仍未确认（驱动层面）

**`pinctrl_lookup_state(dev, "gpio")` 是否真的成功。**
设备树里有属性 ≠ 驱动用上了。决定性证据是坑 4 里那段 dyndbg 输出。

### 长跑观察

跑过 1 小时无瘫痪。但按坑 4 的道理，**这不算证明**。

### 判据

- 好：1 小时 + 真实复现场景（触摸屏有人操作）不瘫痪
- 更好：开 dyndbg 跑到复现，dmesg 里能看到恢复成功
- 最硬：`dmesg` 里检索到 `SCL is stuck low` 之外的恢复记录

---

## 8. 还没闭环的：Ctrl+C 失效

**同一条时间线上另一个没解决的问题，但已与原假设脱钩。**

原假设链：`I2C 卡死 → 线程进 D 状态 → 信号投递不进 → Ctrl+C/Ctrl+Z 全失效`。

推翻它的证据：那次跑 1 小时**没有**总线瘫痪，Ctrl+C 依然不灵。
所以 I2C 至少不是全部原因（甚至可能无关）。

**从头到尾没真正抓到过卡住时的内核栈。** 这是最该补的一步：

```bash
PID=$(pidof health_monitor)          # 注意别用 pgrep -f，会多匹配到 sudo 父进程
ps -L -o lwp,stat,wchan:32,comm -p $PID   # 看哪个线程是 D，卡在哪个内核函数
for t in /proc/$PID/task/*; do sudo cat $t/stack; done
echo w | sudo tee /proc/sysrq-trigger; sudo dmesg | tail -150
```

`wchan` 的头号嫌疑：`__mutex_lock`（`i2c_transfer` 里拿 `adap->bus_lock`，
`mutex_lock` 的等待状态是 `TASK_UNINTERRUPTIBLE`，比总线本身更难恢复）。
次要嫌疑：本版 4.19 的 `i2c_imx_xfer()` 每次传输都做一对
`pm_runtime_enable/disable`（上游后来挪到 probe 了），`pm_runtime_disable()` 内部有 barrier。

---

## 9. 后续工程响应：看门狗兜底

总线问题治不了根（从机为什么会卡死，还没定位到具体触发条件），
所以工程上加了一层兜底：`src/watchdog.c`，硬件看门狗 + 线程心跳监视。

- 监视 5 路心跳（max30102 / mpu6050 / display / tcp / http），
  **故意排除 mqtt** —— `mosquitto_connect()` 阻塞几十秒是正常重连，
  算进去会让网络抖动变成整板重启
- 阈值 20 秒停滞 → 停止喂狗 → 30 秒后 `imx2_wdt` 复位整板
- 跨重启记账：`unclean_boots`（靠自己写 `last_clean` 标记，不依赖硬件）
  + `wd_resets`（靠 `WDIOC_GETBOOTSTATUS`）

**要连起来看的一点**：如果以后给 I2C 访问加指数退避，
退避导致的线程停滞**不能超过看门狗阈值**，否则总线抖一下就把板子喂重启了。
两处必须一起设计。

（看门狗自身的实现要点、`imx2_wdt` 的三条反直觉事实，见
`docs/` 之外的记忆文件和 `include/watchdog.h` 注释。）

---

## 10. 面试追问 Q&A

**Q：你怎么知道是仲裁丢失，而不是别的 I2C 错误？**
A：看 `errno`。这个驱动里 `i2c_imx_bus_busy()` 的 `-EAGAIN` 只有 IAL 一个出处
（grep 全文件确认过），另一种死法"总线一直忙"返回的是 `-ETIMEDOUT`（110）。
拿到 11 就能精确定位到 `I2SR_IAL` 这一位。

**Q：单主机系统怎么会仲裁丢失？**
A：I2C 是开漏线与总线，设备只能拉低。主机释放 SDA（松手）后读回低电平，
控制器只能判定"有别人在驱动"。单主机系统里那个"别人"就是**卡住的从机**——
它内部移位寄存器失去同步，认为字节没传完，把 SDA 拉死不放。

**Q：为什么不直接软件复位 I2C 控制器？**
A：复位控制器能清掉 `I2SR` 里的标志位，但**物理层的僵局没解开**，
从机还在拉 SDA，下一次 START 立刻又 IAL。要解决必须作用在物理层——
也就是敲时钟让从机自己松开。这就是行业里通用做法（I2C 规范 §3.1.16 Bus clear）。

**Q：为什么是 9 个时钟？**
A：一个字节 8 位 + 1 位 ACK = 9 拍。从机最多再差 9 拍就能走完当前字节，
进入能识别 STOP 的状态并释放 SDA。内核里就是 `RECOVERY_CLK_CNT = 9`。

**Q：为什么要切成 GPIO？控制器自己发不了这 9 个时钟吗？**
A：控制器已经认为"总线不归我管"（IAL 就是放弃总线的意思），
它的状态机卡在错误态，不会再自主产生时钟。
绕开控制器、直接操纵引脚电平是唯一可靠的路子——这就是"bitbang"。

**Q：pinctrl 为什么不能少？**
A：pad 默认 mux 到 I2C 功能，此时引脚归 I2C 控制器驱动，
GPIO 子系统碰不到它。必须先 `pinctrl_select_state("gpio")`
把两个 pad 的 mux mode 切到 GPIO，`gpiod_set_value()` 才有作用。
恢复完再切回 `"default"`。

**Q：`pinctrl-names` 里为什么必须字面是 `"gpio"`？**
A：驱动里硬编码 `pinctrl_lookup_state(pinctrl, "gpio")`，
按字符串查。名字不对就是查不到，而且**失败路径是 `dev_dbg`
+ `return 0`**——函数"成功"返回，probe 正常过，功能静默消失。

**Q：你怎么确定补丁真的生效了？**
A：分两层看。
设备树层：`/proc/device-tree/.../pinctrl-names` 输出 `defaultgpio`、
目录里有 `scl-gpios`/`sda-gpios`。
驱动层：开 dyndbg 后 unbind/bind，dmesg 里应有 `using scl,sda for recovery`。
**"跑一小时没出事"不能作为证据**，因为成功路径是 `dev_dbg`，
静默成功和"压根没触发"在日志上长得一模一样。

**Q：这个过程中最大的教训是什么？**
A：**区分"代码没跑到"和"代码跑到了但没输出"。**
失败路径用 `dev_dbg` 是内核里很常见的写法，代价是问题完全静默。
排查时必须先把日志级别打开，否则所有"没有报错"的结论都是无效的。
第二个教训是**运行时状态比源码可信**——中间隔着 dtb、overlay、uEnv.txt 好几层，
overlay 的属性覆盖还是替换语义、失败也不报错。

---

## 附：相关文件位置

| 内容 | 路径 |
|---|---|
| 板级设备树（含补丁） | `arch/arm/boot/dts/imx6ull-mmc-npi.dts` |
| 惹事的 overlay | `arch/arm/boot/dts/overlays/imx-fire-i2c1-overlay.dts` |
| 触摸屏 overlay | `arch/arm/boot/dts/overlays/imx-fire-touch-capacitive-gt1151-overlay.dts` |
| I2C 控制器驱动 | `drivers/i2c/busses/i2c-imx.c` |
| 通用恢复算法 | `drivers/i2c/i2c-core-base.c` |
| 看门狗实现 | `health_monitor/src/watchdog.c` |
| 应用层 I2C 封装 | `health_monitor/src/max30102.c` |
