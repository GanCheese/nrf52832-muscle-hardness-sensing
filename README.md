# nRF52832 双 ICM-20948 + MAX98357A 驱动

本目录是 `nrf52832_肌肉硬度感知` 的 Zephyr 工程，包含：

**当前源码为 IMU1 单路诊断配置**：`app.overlay` 禁用 `icm20948_0`，只启用日志编号 IMU1，即 `icm20948@1`（CS=P0.27、INT=P0.25）。SPI 仍为 SPI1，SCK=P0.31、MOSI=P0.30、MISO=P0.29，1 MHz、mode 0。保留 CS 列表的两个位置，以维持 `reg=1` 对应 P0.27；SPI 控制器将未使用的 P0.28 片选初始化为不选中，不对 IMU0 发起访问。应用只报告启用的设备并保留原编号。单路镜像为 `firmware/imu1_only.signed.bin`；下文双路测试结果属于此前的 `imu_spi_fix.signed.bin`。

**IMU1 单路上板结果（2026-11-06）**：编译通过，经 COM9 上传 47264 字节并校验镜像哈希一致，复位后确认 `reporting 1 enabled device(s)`，没有 IMU0 初始化或采样日志。IMU1 仍报 `bank 2 reg 0x14 = 0x00, expected 0x3c`、`sensor configuration failed: -5`，运行时持续 `IMU1: ready=0 who=0x00 read_ret=0`。日志见 [logs/imu1_only_boot.txt](logs/imu1_only_boot.txt)。这次仅从软件上禁用 IMU0，没有物理断开其模块，不能据此排除未选中模块的电气负载或接线影响。板子当前运行此单路镜像。

- 两个 ICM-20948 共用一条 SPI 总线、独立片选和独立数据就绪中断的驱动；
- MAX98357A I²S 功放驱动（含 SD_MODE 开关机时序）；
- 一个输出固定频率正弦测试音（默认 400 Hz，幅度约 −12 dBFS）、每 2 秒打印两路 IMU 状态和样本的应用程序；
- 对应的 devicetree overlay、binding 和 Kconfig。

工程已在 **nRF Connect SDK v3.4.1 / Zephyr 4.4.2**、`nrf52dk/nrf52832` 目标上通过 MCUboot 单槽 sysbuild 编译，并已通过板载 CH340 的 **COM9** 串口完成 MCUboot SMP 上传和 SHA-256 校验。串口启动日志已确认固件运行，400 Hz 音频输出仍需接喇叭/功放实测。

> 当前板子的 MCUboot 已经预装，日常只更新应用即可走串口；不要把裸 `zephyr.hex` 写到 0x00000，那会覆盖 MCUboot。串口上传必须使用 `zephyr.signed.bin`。
>
> **历史日志（2026-11-06，修复前）**：`C:/nrf_ws/imu_boot_capture3.txt` 中两路都打印 `ready`，随后 `WHO_AM_I=0x00`、样本全 0。该日志不能证明配置成功：SPI 写成功只表示主机完成传输，没有从机应答。
>
> **本次代码检查**：驱动遗漏 `USER_CTRL.I2C_IF_DIS`，并在配置时写入 `USER_CTRL=0x00`。现已在复位后设置并保持 `0x10`，增加上电等待、量程/电源/SPI 模式/中断使能读回及配置后身份校验。依据 [TDK 数据手册](https://invensense.tdk.com/wp-content/uploads/2021/07/DS-000189-ICM-20948-v1.4.pdf) 6.5、8.2 节，SPI 使用时需要禁用 I²C 从接口以防切回 I²C。这是明确的软件遗漏，但尚不能据此确认现场故障已消除。模块的电平和开关问题见 4.2 节；修复后的固件需重新上板验证。

修复版已通过 NCS v3.4.1、`nrf52dk/nrf52832` 完整 MCUboot sysbuild 编译（构建目录 `C:/nrf_ws/build-imu-fix`，应用 FLASH 47252 B、RAM 12032 B）。签名应用为 `firmware/imu_spi_fix.signed.bin`，校验值见 `firmware/SHA256SUMS.txt`；已按用户要求通过 COM9 烧录并测试，结果见下文，原来的音频固件文件仍保留。现有脚本可指定该镜像上传：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\usb-flash.ps1 -SkipBuild -Port COM9 -Image .\firmware\imu_spi_fix.signed.bin
```

**修复版上板结果（2026-11-06）**：47402 字节上传完成，MCUboot 返回的镜像哈希与本地镜像内容哈希一致（`5ac34f6ccd2fb6e88c4df025487e19b855affae5e96ca9d714ba2e45cf9b6ab2`；与整个签名文件的 SHA-256 是不同计算范围）。通过 MCUboot 软件复位捕获的完整应用启动日志保存在 [logs/imu_spi_fix_boot.txt](logs/imu_spi_fix_boot.txt)。

```text
icm20948@0: bank 0 reg 0x03 = 0x00, expected 0x10
icm20948@0: sensor configuration failed: -5
icm20948@1: bank 2 reg 0x14 = 0x00, expected 0x3c
icm20948@1: sensor configuration failed: -5
IMU0: ready=0 who=0x00 read_ret=0
IMU1: ready=0 who=0x00 read_ret=0
```

两路均通过了初始化最初的 WHO_AM_I 检查，但在后续配置读回失败（`-5` 为 `-EIO`），运行时身份持续为 0。修复未消除现场故障，新增校验已阻止误报 ready。日志说明 SPI 通信/配置保持不可靠，不能单凭此确定具体器件损坏；下一步应检查模块电平、SW1、接线与供电，并分离为单模块测试。功放启动及 I2S 写入日志正常，未实测声学输出。按物理 RESET 会进入 MCUboot 恢复模式；本次通过 SMP 软件复位退出恢复并启动应用。

---

## 1. 文件结构

```text
project/
├── CMakeLists.txt
├── Kconfig                      # 固定测试音的频率/幅度选项
├── prj.conf
├── sysbuild.conf                # MCUboot 单槽 + ECDSA P256
├── sysbuild/
│   ├── mcuboot.conf             # UART 恢复、RESET 入口、5 秒等待
│   └── mcuboot.overlay          # CH340 UART 引脚与 boot 分区
├── app.overlay                  # 引脚、SPI、I2S、两个 IMU 和功放节点
├── scripts/
│   ├── usb-flash.ps1            # 编译 + 串口上传
│   └── usb_flash.py             # SMP 握手、上传、哈希校验、复位
├── dts/bindings/
│   ├── sensor/invensense,icm20948.yaml
│   └── audio/maxim,max98357a.yaml
├── firmware/
│   └── max98357a_tone_400hz.signed.bin  # 本次串口上传的签名镜像
├── include/
│   ├── icm20948.h
│   └── max98357a.h
└── src/
    ├── icm20948.c
    ├── max98357a.c
    └── main.c
```

---

## 2. 引脚分配

### 2.1 两个 ICM-20948（SPI 共用）

| nRF52832 | IMU1 | IMU2 | 说明 |
|---|---|---|---|
| P0.31 | SCL/SCLK | SCL/SCLK | SPI SCK，当前 1 MHz，SPI mode 0 |
| P0.30 | SDA/SDI | SDA/SDI | SPI MOSI |
| P0.29 | AD0/SDO | AD0/SDO | SPI MISO |
| P0.28 | CS | 不接 | IMU1 片选，低有效 |
| P0.27 | 不接 | CS | IMU2 片选，低有效 |
| P0.26 | INT1 | 不接 | IMU1 数据就绪，开漏低有效 |
| P0.25 | 不接 | INT1 | IMU2 数据就绪，开漏低有效 |

### 2.2 MAX98357A

| nRF52832 | MAX98357A | 说明 |
|---|---|---|
| P0.05 | BCLK | I²S 位时钟，8 kHz × 16 bit × 2 = 256 kHz |
| P0.04 | LRC/LRCLK | 8 kHz 帧时钟 |
| P0.03 | DIN | I²S 数据 |
| P0.07 | SD/EN | SD_MODE；低=关断，高=左声道 |
| GND | GND | 共地 |
| 独立 3.3 V/5 V 支路 | VIN | 按模块规格供电 |

不连接 MCLK。MAX98357A 不需要 MCLK。

P0.05/P0.07 在 DK 上默认是 UART RTS/CTS；`app.overlay` 已把 UART0 的 pinctrl 精简为只保留板载 CH340 使用的 **TX=P0.08、RX=P0.06**，因此这两个脚可作 I²S 和 SD_MODE。UART 不使用硬件流控。注意这与 nRF52 DK 默认的 P0.06=TX / P0.08=RX 相反，接错会导致 MCUboot 和日志都不通。

---

## 3. 与 `实现方案_模块连接与调试.md` 的引脚差异

方案文档中的 P0.25/P0.26/P0.27/P0.28/P0.29 分配没有采用。本次要求把 IMU 放在 **P0.00–P0.08 / P0.25–P0.31**，并使用单条 SPI。

但 P0.00–P0.08 已经没有可用的高速信号脚：

- P0.00/P0.01：DK 的 32.768 kHz 晶振 XL1/XL2；
- P0.02：AREF；
- P0.03、P0.04、P0.05、P0.07：MAX98357A 的 DIN、LRCLK、BCLK、SD_MODE；
- P0.06、P0.08：UART RX/TX（板载 CH340，MCU TX=P0.08、RX=P0.06）。

因此 7 根 IMU 信号全部放在 **P0.25–P0.31**：

- IMU：P0.25/P0.26/P0.27/P0.28/P0.29/P0.30/P0.31；
- 功放：P0.03/P0.04/P0.05/P0.07。

其中：

- 使用 **SPI1**，其默认引脚正好是 P0.31/SCK、P0.30/MOSI、P0.29/MISO；overlay 只增加两条 CS 和两个 INT。
- **P0.26、P0.27 在 DK 上是 I2C0 的 SDA/SCL**。overlay 已关闭 `&i2c0`；这两根线上的 4.7 kΩ 外部上拉对 INT 和 CS 都是合适的。
- P0.25 是 Arduino D13；P0.28–P0.31 是 A2–A5。它们都可作普通 GPIO/SPI。
- `&spi2` 也已关闭，避免它继续占用 P0.25/P0.23/P0.24 和 P0.22。
- 没有使用 P0.13–P0.16（按键，带去抖电容）、P0.17–P0.20（LED）、P0.21（RESET）、P0.09/P0.10（NFC）。
- `app.overlay` 按 `nrf52dk/nrf52832` 编写，使用了 `&spi1`、`&i2s0`、`&uart0_default` 等板级 label；换其他板子时需要按实际 devicetree 调整。

---

## 4. 必须注意的硬件问题

### 4.1 MAX98357A 的 LRCLK 只能是特定频率

MAX98357A 数据手册第 19 页明确写明：

> LRCLK ONLY supports 8 kHz, 16 kHz, 32 kHz, 44.1 kHz, 48 kHz, 88.2 kHz, and 96 kHz frequencies. LRCLK clocks at 11.025 kHz, 12 kHz, 22.05 kHz and 24 kHz are NOT supported.

**方案文档第 8 节的 `LRCK = 15625 Hz` 不在支持列表内，不能直接使用。** 15625 Hz 既不是 MAX98357A 支持的频率，也不是 nRF52832 内部 32 MHz 时钟能精确产生的标准音频频率。

本工程改用 **8 kHz**，原因：

```text
nRF52832 I2S: MCK = 32 MHz / 125 = 256 kHz
MCK / LRCK = 32
LRCK = 256 kHz / 32 = 8 kHz
BCLK = 8 kHz × 16 bit × 2 ch = 256 kHz
```

8 kHz 的 Nyquist 频率是 4 kHz，覆盖本项目 50–800 Hz 的激励/分析频带。若后续必须使用 16 kHz 或更高标准音频率，需要外部音频时钟或换用支持分数分频的 MCU，不能只改 `sample-rate`。

`app.overlay` 中已经写死 `sample-rate = <8000>`。`max98357a.c` 也只为 16-bit PCM 实现。

### 4.2 ICM-20948 的 VDDIO 电平

ICM-20948 裸芯片 VDDIO 为 **1.71–1.95 V**。GY-ICM20948V2 模块是否带电平转换、是否能把 SCLK/SDI/SDO/CS/INT 直接接到 3.3 V MCU，必须对照模块原理图确认。

本仓库的 `GY-20948V2-SCH.png` 显示：

- U3 `ME6206A-18` 产生 **1.8 V VCC**，ICM-20948 的 VDDIO 接这个 1.8 V；
- `Q1` 只对 **SCL/SDA** 两路做电平转换；
- **nCS** 通过 R1 10 kΩ 上拉到 1.8 V，直接进模块；
- **SDO/AD0** 直接从 ICM-20948 的 1.8 V 输出引出；
- SCLK/SDI 经过 Q1，尚可接受 3.3 V 输入。

因此该模块**不是完整的 3.3 V SPI 模块**。3.3 V 的 nCS 不符合芯片 VDDIO 电平要求，SDO 的 1.8 V 高电平也不能保证被 3.3 V 供电的 nRF52832 识别。硬件问题与软件遗漏可能同时存在；历史日志不足以断定唯一原因。

原理图还有两项必须核对实际模块的设置：

- **SW1 连接 AD0/SDO 与 1.8 V VCC**。SPI 使用 AD0 作为 MISO 输出，SW1 必须断开，不能将该输出固定接电源；板上的 R5 10 kΩ 下拉也应纳入电平转换的负载检查。
- **INT 没有电平转换，且有 R4 10 kΩ 下拉**。当前固件采用开漏低有效，不能只依赖 MCU 内部弱上拉。应按模块实际电路处理下拉，并在传感器侧上拉至 1.8 V，再转换到 MCU 电平。

处理方式：

- 使用适合 SPI 速率及实际负载的电平转换方案；nRF 侧 3.3 V、芯片侧 1.8 V，按方向处理 SCK/MOSI/CS 与 MISO/INT。SCL/SDA 已有板载 MOS 转换，不能盲目级联；两路独立 CS 和 INT 都要覆盖；
- 或换用明确带 3.3 V 电平转换的 ICM-20948 模块（例如 SparkFun 9DoF ICM-20948）；
- 不要让 3.3 V 直接驱动芯片侧的 nCS/SDO/INT。模块的 I2C 侧有 Q1 电平转换，可用低速 I2C 单模块检查身份，但这需要另外的测试固件和接线。

如果只接一个 IMU，CS 只能接 P0.28（IMU0）或 P0.27（IMU1）其中一个，另一个保持不接。**不能把两路 CS 并到同一个模块的 CS 脚**：两个 GPIO 会同时驱动同一根线，平时一个输出高、一个输出低，形成短路和不定电平。当前实测两个驱动都打印 ready，若实际只接了一个模块，应优先检查这种 CS 并线/短接。

### 4.3 片选上电默认电平

IMU 的 CS 默认应为高。nRF52832 复位后 GPIO 为输入，模块上的 CS 若悬空可能被拉低。建议：

- 在每条 CS 上加 10 kΩ 上拉到模块 VDDIO；或
- 确认模块板载已有上拉。

否则可能出现“上电时 CS 为低，芯片进入不支持的状态”的问题。ICM-20948 手册明确说明上电时 CS/SCLK 同时为低不是受支持的使用场景。

### 4.4 INT 电气形式

本工程把 INT1 配置为：

- 开漏输出；
- 低有效；
- 锁存，直到任意寄存器读操作清除（`INT_ANYRD_2CLEAR`）。

所以 INT 线需要上拉。overlay 给 P0.26/P0.25 启用了 MCU 内部上拉，但 GY 模块原理图中的 R4 10 kΩ 下拉会影响高电平；内部上拉不能替代正确的硬件处理。芯片侧应使用 1.8 V 上拉及适当电平转换（见 4.2 节）。本工程以 DK 的设备树适配实际 CH340 板，不能假设实际板上存在 DK 的外部上拉。overlay 已关闭 `&i2c0`。

### 4.5 MAX98357A 的 SD_MODE

SD_MODE 不是普通使能脚，它是多电平输入：

| SD_MODE 电压 | 模式 |
|---|---|
| 低（< B0） | 关断 |
| 高（> B2） | 左声道 |
| 通过 RSMALL 上拉 | 右声道 |
| 通过 RLARGE 上拉 | (左/2 + 右/2) |

本工程用 GPIO 推挽驱动 SD_MODE，高电平选择**左声道**。因此测试音在左右声道写相同数据，左声道输出即可。

上电前确认模块 SD 脚没有不可覆盖的强上拉/分压；如果模块已经把 SD_MODE 固定成某种模式，应去掉该固定电路或改由 GPIO 控制，否则驱动可能无效或短路。

### 4.6 功放输出

- SPK+ / SPK− 是差分输出，**SPK− 不是地**。
- 不能把任一喇叭端接系统地，也不能把两个功放输出并联。
- 用示波器测差分输出时不要用接地夹夹任一输出端。
- ASX02604-R 从低幅度开始，检查温升和失真。

---

## 5. 驱动配置

### 5.1 ICM-20948

`app.overlay` 中每个 IMU 的配置：

```dts
spi-max-frequency = <1000000>;   /* 当前诊断值；电平修好后可回到 4000000 */
accel-range-g = <8>;
gyro-range-dps = <500>;
accel-dlpf-bypass;          /* 加速度 DLPF 旁路 */
gyro-dlpf-cfg = <7>;
gyro-sample-rate-div = <0>;
```

对应寄存器效果：

| 项目 | 值 |
|---|---|
| 加速度量程 | ±8 g，4096 LSB/g |
| 加速度 DLPF | 旁路，ODR 4500 Hz，−3 dB 约 1209 Hz |
| 陀螺量程 | ±500 dps，65.5 LSB/dps |
| 陀螺 DLPF | cfg 7，−3 dB 约 361 Hz |
| 陀螺 ODR | 1125 Hz |
| 工作模式 | 低噪声（`LP_CONFIG = 0`） |
| 数据读取 | 从 0x2D 起连续读 14 字节：加速度 6 + 陀螺 6 + 温度 2 |
| 中断 | INT1，原始数据就绪，开漏低有效，锁存，任意读清除 |

初始化流程：等待上电稳定 100 ms → 软复位 → 等 100 ms → 设置 SPI-only（`USER_CTRL=0x10`）→ 读 WHO_AM_I（应为 0xEA）→ 唤醒、使能全部轴、配置量程/DLPF/ODR → 读回量程、电源和 SPI 模式 → 配置 INT1 → 等待 50 ms 并复查身份 → 注册 GPIO 中断 → 打开并读回数据就绪使能。任一校验失败均返回错误，日志含设备名、bank、寄存器和期望值。

排查顺序：先只接一路模块并确认电平转换与 SW1 设置，检查 `WHO=0xEA` 和配置读回；再接第二路（CS 分别为 P0.28/P0.27，不能并接）。两路身份稳定后检查 `irq` 是否增长。SPI 的 `read_ret=0` 并不保证从机在线；`WHO=0x00/0xff` 仍属于通信异常。当前每 2 秒只取一次样本，用于诊断，不是 4500 Hz 连续采集。

### 5.2 MAX98357A

驱动启动顺序：

```text
i2s_configure(8 kHz, 16-bit, stereo, I2S, master)
  -> 预装 3 个静音块
  -> I2S_TRIGGER_START（时钟开始）
  -> 等 1 ms
  -> SD_MODE 拉高（功放退出 shutdown）
```

停止顺序：

```text
SD_MODE 拉低（先关功放）
  -> 等 1 ms
  -> I2S_TRIGGER_DROP（再停 BCLK/LRCK）
```

这样避免“BCLK 还在、LRCLK 消失”导致功放输出大直流。手册明确要求不要在 BCLK 存在时移除 LRCLK。

---

## 6. API

### ICM-20948

```c
#include "icm20948.h"

const struct device *imu = DEVICE_DT_GET(DT_NODELABEL(icm20948_0));
struct icm20948_sample sample;

if (!device_is_ready(imu)) {
    /* 初始化失败，查看串口日志 */
}

if (icm20948_read_wait(imu, &sample, K_MSEC(10)) == 0) {
    float ax_g = sample.accel[0] / icm20948_accel_lsb_per_g(imu);
    float gx_dps = sample.gyro[0] / icm20948_gyro_lsb_per_dps(imu);
    float temp_c = icm20948_temp_to_c(sample.temperature);
    uint32_t t = sample.timestamp;  /* k_cycle_get_32()，32768 Hz */
}
```

- `icm20948_read_wait()`：等一个数据就绪边沿再读；
- `icm20948_read()`：立即读，不等待；
- `icm20948_wait_data()`：只等边沿，不读；
- `icm20948_get_stats()`：读 IRQ/样本/溢出/错误计数；
- `icm20948_get_who_am_i()`：身份检查；
- `icm20948_accel_lsb_per_g()` / `icm20948_gyro_lsb_per_dps()`：按当前量程换算；
- `icm20948_temp_to_c()`：原始温度转 ℃。

`timestamp` 是中断里取的 `k_cycle_get_32()`，分辨率约 30.5 µs（32768 Hz），会回绕。它只用于本地节拍统计，不是硬件时间戳；两个 IMU 也不共用同一个采样时钟。需要精确相位时，后续应改成 1 MHz 定时器或 GPIOTE/PPI 捕获。

### MAX98357A

```c
#include "max98357a.h"

const struct device *amp = DEVICE_DT_GET(DT_NODELABEL(max98357a));
int16_t frames[128 * 2];  /* 左右交错，16-bit */

max98357a_start(amp);
/* 每个 block 最多 max98357a_max_frames() 帧，512 字节 */
max98357a_write(amp, frames, 128);
max98357a_stop(amp);
```

- `max98357a_start()`：配置 I²S、预装静音、启动时钟、开功放；
- `max98357a_write()`：写入一个 block，数据会被复制，函数可能在 TX 队列满时阻塞；
- `max98357a_stop()`：先关功放再停 I²S；
- `max98357a_sample_rate()`：返回 8000；
- `max98357a_max_frames()`：返回 128（512 字节 / 4 字节每帧）；
- `max98357a_enable()`：仅直接控制 SD_MODE，调试用。

当前 `main.c` 启动功放并持续写入固定频率正弦波，同时每 2 秒报告两路 IMU；功放初始化失败也继续报告 IMU。IMU 在 Zephyr 设备初始化阶段配置，不接收运行时命令。频率和幅度在编译时由 Kconfig 固定：

| 选项 | 默认值 | 说明 |
|---|---:|---|
| `CONFIG_APP_TONE_FREQ_HZ` | 400 | 固定音频率，范围 20–2000 Hz |
| `CONFIG_APP_TONE_AMPLITUDE` | 8000 | 16-bit PCM 峰值，约 −12 dBFS |

改频率/幅度后重新编译：

```bash
west build -b nrf52dk/nrf52832 -d build . \
  -- -DCONFIG_APP_TONE_FREQ_HZ=1000 -DCONFIG_APP_TONE_AMPLITUDE=12000
```

相位累加器在 block 之间保持连续；`tone_thread` 每块 128 帧（16 ms）。8 kHz 采样率下 400 Hz 每周期 20 个采样点。若写 I²S 失败并返回 `-EIO`，线程会先关功放、再重新启动 I²S，而不是让功放停在无时钟状态。

---

## 7. 编译和烧录

本工程按 nRF Connect SDK v3.4.1 的板名 `nrf52dk/nrf52832` 编译。较旧的 SDK 可能是 `nrf52dk_nrf52832`，按实际 `west boards` 结果替换。

### 7.1 日常串口烧录（推荐）

板载 MCUboot 已预装，应用槽从 `0x0C000` 开始。编译必须使用 `--sysbuild`，生成带签名的 `zephyr.signed.bin`，不能上传从 0x00000 开始的裸应用。

在工程目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\usb-flash.ps1 -Port COM9
```

脚本会：

1. 通过 NCS v3.4.1 在 `C:\nrf_ws\build-usb` 做 MCUboot sysbuild；
2. 打开 CH340 串口，等待 MCUboot 的 SMP 握手；
3. 上传 `build-usb/project/zephyr/zephyr.signed.bin`；
4. 读取 slot 0 的镜像哈希并与本地 SHA-256 对比；
5. 发送软件复位；约 5 秒后应用启动。

如果板子当前运行的是应用而不是 MCUboot，脚本会打印：

```text
Waiting for MCUboot on COM9. Press RESET now.
```

此时按一下板上 **RESET**。烧录成功时输出类似：

```text
MCUboot handshake OK
Upload: 46107/46107 (100%)
Slot 0: version=0.0.0, hash=6fcca4e5...
Image SHA-256 verified
Upload finished; device reset. Application starts after 5 seconds.
```

只上传已有编译产物：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\usb-flash.ps1 -Port COM9 -SkipBuild
```

改代码后不要加 `-SkipBuild`，否则上传的是旧镜像。

### 7.2 SWD/J-Link 路径（仅首次安装/恢复 Bootloader）

`west flash` 通过 SWD 访问 nRF52832，不是串口烧录。当前机器没有连接 J-Link，日常更新不需要它。

首次安装或恢复 MCUboot 时，用 J-Link 烧录：

```powershell
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 --chdir C:/ncs/v3.4.1 -- `
  west flash -d C:/nrf_ws/build-usb --dev-id <J-Link序列号>
```

不要把日常应用通过 SWD 写到 `0x00000`，否则会覆盖 MCUboot。

### 7.3 中文路径注意事项

本工程所在路径包含中文（`D:\document\nrf52832_肌肉硬度感知`）。NCS/Zephyr 在含中文的构建路径上会出现路径拆分错误，因此本工程使用 ASCII 目录联接（junction）：

```powershell
New-Item -ItemType Junction -Path C:\nrf_ws\project `
  -Target "D:\document\nrf52832_肌肉硬度感知\project"

cd C:\nrf_ws\project
west build --sysbuild -b nrf52dk/nrf52832 -d C:\nrf_ws\build-usb .
```

`scripts/usb-flash.ps1` 已自动优先使用 `C:\nrf_ws\project` 作为源码目录、`C:\nrf_ws\build-usb` 作为构建目录。

### 7.4 验证结果

本次 MCUboot sysbuild 结果：

```text
应用 FLASH: 46624 B / 225114 B (20.71%)
应用 RAM:   12032 B / 64 KB (18.36%)
MCUboot FLASH: 31072 B / 48 KB (63.22%)
签名镜像: 46776 B
```

串口烧录和启动日志已实机验证：

```text
MCUboot handshake OK
Slot 0: version=0.0.0, hash=6fcca4e5a2d1cb49e422d87da6c53170fb7a75e68c968be7b8cfcf0a55f51cfe
Image SHA-256 verified
...
*** nRF52832 MAX98357A fixed-tone test ***
Tone: 400 Hz, amplitude 8000/32767, LRCLK: 8000 Hz
AMP: playing 400 Hz at 8000 Hz sample rate, amplitude 8000
```

这说明应用镜像、MCUboot 启动、UART 日志和 I²S 初始化都已跑通。是否真的听到 400 Hz，还取决于功放供电、SD_MODE 电平和喇叭接线。

日志中如果出现两条 `icm20948: unexpected WHO_AM_I 0x00`，表示当前没有接 IMU；IMU 驱动仍会在启动时探测，但不影响功放线程。

### 7.5 串口上传文件

MCUboot 串口恢复只接受签名镜像，不能用 HEX 改名：

```text
C:\nrf_ws\build-usb\project\zephyr\zephyr.signed.bin
project\firmware\max98357a_tone_400hz.signed.bin
```

`project\firmware\max98357a_tone_400hz.signed.bin` 是本次成功上传的诊断固件副本，文件 SHA-256：

```text
DE604AB446368718ADEA63099F2BE1CB2C2AD3CD7BD99071457EA97202512696
```

最近一次 MCUboot slot 0 镜像哈希：

```text
f51708df1c16106637ad026b8537788c0f4c1d83a9f62a5d901d3ff2612dd2a8
```

---

## 8. 上电调试顺序

当前固件只启动 MAX98357A，建议先按下面顺序单独验证音频，不要一次全接：

1. **先不接喇叭**，只接功放电源、GND 和 SD_MODE，确认 SD 电平在启动后为高；
2. 示波器/逻辑分析仪看 P0.05 BCLK、P0.04 LRCK、P0.03 DIN：LRCK 应为 8 kHz，BCLK 应为 256 kHz；
3. 接 ASX02604-R，幅度从 `CONFIG_APP_TONE_AMPLITUDE=3000` 开始，确认能听到 400 Hz 单音；
4. 逐步提高幅度，检查功放温升、电源跌落和失真；
5. 音频稳定后，再单独接 IMU，按 `WHO_AM_I=0xea`、静态加速度、数据就绪的顺序验证；当前 `main.c` 不会自动启动 IMU，需要恢复 IMU 线程或另写测试程序；
6. 最后再做双 IMU 和宽带激励。

调试时 `CONFIG_LOG_MODE_IMMEDIATE=y` 会占用较多 CPU，量产固件可关闭日志或改为 deferred。

---

## 9. 当前限制

- **当前固件只输出固定单音**。`main.c` 不启动 IMU、不记录原始数据，也不做 50–800 Hz 多频合成、频散反演或串口/BLE 数据流；IMU 驱动只是保留在工程里。
- **固定频率/幅度在编译时确定**。没有运行时命令接口；要改频率必须重新编译。
- **没有实现 FIFO 批量读取**。4 MHz SPI 下，两个 4500 Hz 传感器约 9 k 次/秒事务，纯数据位时间已占总线约 27%（15 字节/次 × 8 bit ÷ 4 MHz × 9000 次/秒），再加 CS/软件开销。若 `ovr` 持续增加，应改用 ICM-20948 FIFO 批量读出，而不是简单提高 SPI 频率。**两颗 IMU 也可以分到两条独立 SPI 总线上**，但瓶颈通常在传感器 4500 Hz 的加速度 ODR、中断/线程开销和每字节软件处理，而不是共享总线的 4 MHz 带宽；分离总线不会把加速度 ODR 提到 4500 Hz 以上。
- **两个 IMU 不同步**。各自内部时钟独立，时间戳只来自各自中断。论文要求的双通道相位校准仍需在电脑端完成。
- **时间戳分辨率 30.5 µs**。对 800 Hz（周期 1.25 ms）约为 2.4% 周期，做精确相位前应改用更高分辨率计时。
- **串口烧录已跑通，但依赖预装 MCUboot**。日常更新上传 `zephyr.signed.bin`；若 MCUboot 被擦除，需要先用 J-Link/SWD 恢复。
- **IMU 当前读不到数据**。GY-ICM20948V2 的 SPI 侧为 1.8 V 逻辑（nCS/SDO 未转换），与 3.3 V nRF52832 不兼容。实测驱动初始化成功，但随后 `WHO_AM_I=0x00`、三轴数据全 0。需要加电平转换或更换 3.3 V 兼容模块。
- **未做音频硬件在环验证**。串口日志确认固件和 I²S 已启动，但 SD_MODE 电平、8 kHz I²S 波形、MAX98357A 输出和喇叭声音仍需按实物核对。

---

## 10. 参考

- ICM-20948 数据手册 DS-000189 v1.3（本仓库 `GY-ICM20948V2资料/DS-000189-ICM-20948-v1.3.pdf`）
- MAX98357A/B 数据手册（本仓库 `MAX98357_资料/MAX98357 datasheet.pdf`），重点：第 19 页 LRCLK 限制、Table 5 SD_MODE
- Zephyr I²S API：`zephyr/drivers/i2s.h`
- Zephyr SPI API：`zephyr/drivers/spi.h`
