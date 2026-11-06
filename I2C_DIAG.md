# ICM-20948 I2C 单模块诊断

接线：P0.31 -> SCL，P0.30 -> SDA，共地，模块输入供电 3.3 V。
断开 MCU 与模块 CS、AD0/SDO、INT 的连接。CS 使用模块板载 1.8 V 上拉。
SW1 断开时预期地址为 0x68；程序同时检查 0x69。
IMU0 保持物理断开。本诊断不启动音频。

构建（PowerShell）：
```powershell
$env:PYTHONUTF8='1'
& C:/ncs/bin/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir C:/ncs/v3.4.1 -- west build --sysbuild -b nrf52dk/nrf52832 -d C:/nrf_ws/build-i2c-diag C:/nrf_ws/project -- '-DEXTRA_CONF_FILE=i2c_diag.conf' '-DEXTRA_DTC_OVERLAY_FILE=i2c_diag.overlay'
```

仅上传签名应用，保留原有 MCUboot。每 2 秒检查两个地址，打印传输返回码和身份；身份为 0xEA 时复位、唤醒并读取六轴原始值。
未应答地址返回负错误码属于预期；有效地址应持续返回 0xEA，且六轴值随姿态和运动变化。
默认 SPI 工程不受此可选诊断配置影响。恢复 SPI 固件前应断电恢复接线。

## 实板结果（2026-11-06）
已上传 imu_i2c_diag.signed.bin 并通过 MCUboot 镜像哈希校验。
上传后仅复位 MCU 时，0x68/0x69 均 ret=-5；整板与传感器断电重启后，0x68 连续返回 WHO_AM_I=0xEA，并成功读取变化的六轴原始数据。0x69 未响应符合单模块地址配置。
日志：logs/imu_i2c_powercycle.txt。
这证明该模块的 I2C 通信及基本六轴读取可用；尚未验证运动响应、校准与高采样率性能，也不能由此确定 SPI 失效的唯一原因。
此前 SPI 固件禁用 I2C 接口，切换为 I2C 诊断时需对传感器断电重启。
