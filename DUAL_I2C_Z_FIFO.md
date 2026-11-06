# 双独立 I2C / Z 轴加速度（FIFO 版）

这是 [DUAL_I2C_Z.md](DUAL_I2C_Z.md) 中轮询版的提速版本。接线、引脚和传感器量程完全相同，只把“读状态 + 读 Z”的逐样本事务改成批量读取硬件 FIFO。

| 模块 | 控制器 | SCL | SDA | 地址 |
|---|---|---|---|---|
| IMU0 | TWIM0 | P0.29 | P0.28 | 0x68 |
| IMU1 | TWIM1 | P0.31 | P0.30 | 0x68 |

## 实现

- Bank 0：`FIFO_CFG(0x76)=0` 选择单 FIFO，`FIFO_RST(0x68)=0x1F` 后清零，`FIFO_EN_2(0x67)=0x10` 只开加速度，`FIFO_MODE(0x69)=0` 流模式，最后置 `USER_CTRL(0x03)` 的 `FIFO_EN`。
- 单 FIFO 配置下每个加速度样本是 6 字节 `X/Y/Z` 大端，无包头；Z 取每组的第 5、6 字节。
- 每 2 ms 读一次 `FIFO_COUNT`，只取 6 的整数倍，单次最多 252 字节（42 样本），避免超过 nRF TWIM 的 256 字节 EasyDMA 上限。
- 采样率按成功解析的样本数统计，不按 FIFO 字节数估算。

## 上板结果（2026-11-06）

- 经 CH340 **COM7** 上传 39303 字节，MCUboot 镜像哈希校验通过。
- 两路 `WHO_AM_I=0xEA`、`ready=1`、`errors=0`。
- 实测 **4528–4551 samples/s**（约 4500 Hz，传感器标称 ODR 上限），比轮询版的 1160–1190 Hz 提高约 3.9 倍。
- FIFO 峰值 330/512 字节，未溢出，余量约 180 字节（约 6.7 ms）。
- 原始日志：[logs/dual_i2c_fifo_boot.txt](logs/dual_i2c_fifo_boot.txt)。
- 镜像：[firmware/dual_i2c_z_fifo.signed.bin](firmware/dual_i2c_z_fifo.signed.bin)，SHA-256 `D62135BA00ABD7AF6CF69CCED12A959CCF8398C42843A653D04D57D93B8CFFB4`。

## 边界

- 两路各自使用独立 FIFO 和独立时钟域，计数会有少量差异；**不是硬件同步采样**，不能直接用于要求严格同时刻的相位差，除非增加同步触发或后处理对齐。
- 当前只保留最新 Z 和累计样本数，不保存完整波形；FIFO 溢出时仍会静默丢样，`fifo_max` 可用于观察余量。
- 4500 Hz 已是 ICM20948 加速度 ODR 上限，继续提速只能减少每样本字节数或改用 SPI；400 kHz I²C 下没有更大空间。

## 构建

```powershell
$env:PYTHONUTF8='1'
& C:/ncs/bin/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir C:/ncs/v3.4.1 -- west build --sysbuild -b nrf52dk/nrf52832 -d C:/nrf_ws/build-dual-i2c-z-fifo C:/nrf_ws/project -- '-DEXTRA_CONF_FILE=dual_i2c_z_fifo.conf' '-DEXTRA_DTC_OVERLAY_FILE=dual_i2c_z.overlay'
```
