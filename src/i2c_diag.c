#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static int configure(uint16_t addr)
{
	int ret = i2c_reg_write_byte(bus, addr, 0x06, 0x80);
	if (ret) return ret;
	k_msleep(100);
	ret = i2c_reg_write_byte(bus, addr, 0x7f, 0);
	if (ret) return ret;
	ret = i2c_reg_write_byte(bus, addr, 0x06, 0x01);
	if (ret) return ret;
	ret = i2c_reg_write_byte(bus, addr, 0x07, 0);
	if (ret) return ret;
	k_msleep(50);
	uint8_t value;
	ret = i2c_reg_read_byte(bus, addr, 0x06, &value);
	if (ret) return ret;
	return value == 0x01 ? 0 : -EIO;
}

int main(void)
{
	bool configured[2] = { false, false };
	printk("ICM20948 I2C diagnostic: SCL=P0.31 SDA=P0.30 100kHz\n");
	if (!device_is_ready(bus)) {
		printk("I2C controller not ready\n");
		return 0;
	}
	k_msleep(100);
	while (true) {
		for (uint16_t addr = 0x68; addr <= 0x69; addr++) {
			uint8_t id = 0;
			int ret = i2c_reg_write_byte(bus, addr, 0x7f, 0);
			if (!ret) ret = i2c_reg_read_byte(bus, addr, 0, &id);
			printk("I2C 0x%02x: ret=%d WHO_AM_I=0x%02x\n", addr, ret, id);
			if (ret || id != 0xea) {
				configured[addr - 0x68] = false;
				continue;
			}
			if (!configured[addr - 0x68]) {
				ret = configure(addr);
				printk("I2C 0x%02x: wake/config ret=%d\n", addr, ret);
				if (ret) continue;
				configured[addr - 0x68] = true;
			}
			uint8_t raw[12];
			ret = i2c_burst_read(bus, addr, 0x2d, raw, sizeof(raw));
			if (ret) {
				printk("sample ret=%d\n", ret);
				continue;
			}
			int16_t v[6];
			for (int i = 0; i < 6; i++) v[i] = (int16_t)((raw[2*i] << 8) | raw[2*i+1]);
			printk("raw accel=%d,%d,%d gyro=%d,%d,%d\n", v[0], v[1], v[2], v[3], v[4], v[5]);
		}
		k_msleep(2000);
	}
	return 0;
}
