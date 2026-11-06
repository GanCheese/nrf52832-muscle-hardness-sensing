# 双独立 I2C / Z 轴加速度

| 模块 | 控制器 | SCL | SDA | 地址 |
|---|---|---|---|---|
| IMU0 | TWIM0 | P0.29 | P0.28 | 0x68 |
| IMU1 | TWIM1 | P0.31 | P0.30 | 0x68 |

两模块输入供电 3.3 V，共地，SW1 均断开。CS 使用模块板载 1.8 V 上拉，CS/AD0/INT 不接 MCU。更换 SPI 固件后需要模块完全断电，以清除 I2C_IF_DIS。

每条总线 400 kHz，各使用一个 TWIM 和独立线程。SPI0/1 禁用以避免外设资源冲突。此模式不启动功放。
初始化：身份及配置读回校验；陀螺关闭，三轴加速度硬件启用，但数据读取仅 Z 轴寄存器 0x31/0x32；±8g、4096 LSB/g、DLPF 旁路（标称 4500 Hz）。

轮询 INT_STATUS_1，看到 DRDY 后读取 Z。每 2 秒打印最新 Z、成功读取数/秒及错误计数。初始化失败自动重试，一路失败不阻塞另一路。

限制：DRDY 是标志而不是样本计数器。统计是成功读取速率，不能证明无丢样或双路同步；当前只保留最新值与计数，不保存完整波形。状态轮询与 Z 读取的总线开销可能限制实际速率。该版本先完成双路连通与吞吐验证，尚不是论文的连续记录/相位分析固件。

构建：
```powershell
$env:PYTHONUTF8='1'
& C:/ncs/bin/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir C:/ncs/v3.4.1 -- west build --sysbuild -b nrf52dk/nrf52832 -d C:/nrf_ws/build-dual-i2c-z C:/nrf_ws/project -- '-DEXTRA_CONF_FILE=dual_i2c_z.conf' '-DEXTRA_DTC_OVERLAY_FILE=dual_i2c_z.overlay'
```

## 上板结果（2026-11-06）

- 经 CH340 **COM7** 上传 38262 字节，MCUboot 返回镜像哈希与本地一致。
- 两路均初始化成功：`WHO_AM_I=0xEA`、`ready=1`、`errors=0`。
- 实测读取速率约 **991 → 1193 reads/s**，稳定在 **1160–1190 Hz**，低于论文的 1600 Hz；当前轮询方式（状态 + Z 两次事务）是主要瓶颈。
- 原始日志：[logs/dual_i2c_boot.txt](logs/dual_i2c_boot.txt)。
