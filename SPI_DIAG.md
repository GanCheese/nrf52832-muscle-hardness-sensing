# SPI 慢速诊断

单 IMU1：SCK=P0.31 MOSI=P0.30 MISO=P0.29 CS=P0.27。
IMU0 物理断开，SW1 断开。CS/MISO 的 1.8 V/3.3 V 电平匹配仍需确认。

spi_diag.overlay 将速率设为 125 kHz，CS setup/hold 各 10 us，mode 0。
驱动在 Bank 选择后增加 10 us 间隔，并增加复位后的 USER_CTRL 校验、8 次身份稳定性校验、唤醒寄存器即时读回。
此配置用于通信诊断，不代表高采样率吞吐量配置。应用每 2 秒轮询读取，不要求连接 INT 才能看到样本。

构建：
```powershell
$env:PYTHONUTF8='1'
& C:/ncs/bin/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir C:/ncs/v3.4.1 -- west build --sysbuild -b nrf52dk/nrf52832 -d C:/nrf_ws/build-spi-diag C:/nrf_ws/project -- '-DEXTRA_DTC_OVERLAY_FILE=spi_diag.overlay'
```

Mode 0 实测：125 kHz 下身份持续 0xEA，但复位后 USER_CTRL 写 0x10 读回 0x00，初始化失败。详见 logs/imu_spi_125k_boot.txt。下一步以 spi_mode3.overlay 单独比较 mode 3。

Mode 3 实测：已上传并校验镜像哈希。启动日志 operation=0x106（mode 3），125000 Hz，WHO_AM_I=0xEA；USER_CTRL 仍读回 0x00（预期 0x10），初始化失败。日志 logs/imu_spi_mode3_boot.txt。当前板子运行 firmware/imu_spi_mode3.signed.bin。两种模式的结果一致，尚不能确认写入失效是电平、波形还是其他原因；需进一步检查 CS/MOSI/供电和电平转换。
