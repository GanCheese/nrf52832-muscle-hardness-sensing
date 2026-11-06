/* Independent buses/threads. Status polling is not a lossless recorder:
 * DRDY is a flag, not a sample counter. No claim of cross-IMU synchronization.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

struct channel {
	const struct device *bus;
	atomic_t ready, count, errors, z;
};
static struct channel channels[2] = {
	{ .bus = DEVICE_DT_GET(DT_NODELABEL(i2c0)) },
	{ .bus = DEVICE_DT_GET(DT_NODELABEL(i2c1)) },
};

static int write_check(struct channel *c, uint8_t reg, uint8_t value)
{
	uint8_t actual = 0;
	int ret = i2c_reg_write_byte(c->bus, 0x68, reg, value);
	if (!ret) ret = i2c_reg_read_byte(c->bus, 0x68, reg, &actual);
	if (!ret && actual != value) ret = -EIO;
	if (ret) printk("%s reg=%02x expected=%02x actual=%02x ret=%d\n",
		c->bus->name, reg, value, actual, ret);
	return ret;
}

static int setup(struct channel *c)
{
	uint8_t id = 0;
	int ret;
	if (!device_is_ready(c->bus)) return -ENODEV;
	ret = write_check(c, 0x7f, 0);
	if (ret) return ret;
	ret = i2c_reg_read_byte(c->bus, 0x68, 0, &id);
	printk("%s WHO_AM_I=0x%02x ret=%d\n", c->bus->name, id, ret);
	if (ret || id != 0xea) return ret ? ret : -ENODEV;
	ret = i2c_reg_write_byte(c->bus, 0x68, 0x06, 0x80);
	if (ret) return ret;
	k_msleep(100);
	if ((ret = write_check(c, 0x7f, 0))) return ret;
	if ((ret = write_check(c, 0x06, 0x01))) return ret;
	k_msleep(10);
	/* All accelerometer axes enabled, gyro disabled; payload is Z only. */
	if ((ret = write_check(c, 0x07, 0x07))) return ret;
	if ((ret = write_check(c, 0x05, 0))) return ret;
	if ((ret = write_check(c, 0x03, 0))) return ret;
	if ((ret = write_check(c, 0x7f, 0x20))) return ret;
	/* +/-8 g, DLPF bypass, nominal accelerometer ODR 4500 Hz. */
	if ((ret = write_check(c, 0x14, 0x04))) return ret;
	if ((ret = write_check(c, 0x7f, 0))) return ret;
	/* Only reading INT_STATUS_1 clears DRDY; no physical INT needed. */
	if ((ret = write_check(c, 0x0f, 0))) return ret;
	if ((ret = write_check(c, 0x11, 1))) return ret;
	k_msleep(50);
	return 0;
}

static void worker(void *arg, void *unused1, void *unused2)
{
	struct channel *c = arg;
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	k_msleep(150);
	while (true) {
		int ret = setup(c);
		if (ret) {
			atomic_inc(&c->errors);
			printk("%s init failed %d; retry in 2s\n", c->bus->name, ret);
			k_msleep(2000);
			continue;
		}
		atomic_set(&c->ready, 1);
		while (true) {
			uint8_t status = 0, raw[2];
			ret = i2c_reg_read_byte(c->bus, 0x68, 0x1a, &status);
			if (!ret && (status & 1)) {
				ret = i2c_burst_read(c->bus, 0x68, 0x31, raw, 2);
				if (!ret) {
					atomic_set(&c->z, (int16_t)((raw[0] << 8) | raw[1]));
					atomic_inc(&c->count);
				}
			}
			if (ret) {
				atomic_inc(&c->errors);
				atomic_set(&c->ready, 0);
				k_msleep(100);
				break;
			}
			k_yield();
		}
	}
}
K_THREAD_DEFINE(imu0_thread, 1536, worker, &channels[0], NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(imu1_thread, 1536, worker, &channels[1], NULL, NULL, 5, 0, 0);

int main(void)
{
	uint32_t previous[2] = {0};
	int64_t last = k_uptime_get();
	printk("DUAL I2C Z: IMU0 SCL=29 SDA=28; IMU1 SCL=31 SDA=30; 400kHz, addr=0x68\n");
	printk("+/-8g, 4096 LSB/g; nominal ODR=4500Hz; polled DRDY, NOT lossless/synchronized\n");
	while (true) {
		k_msleep(2000);
		int64_t now = k_uptime_get();
		for (int i = 0; i < 2; i++) {
			uint32_t count = (uint32_t)atomic_get(&channels[i].count);
			printk("IMU%d ready=%d z=%d reads/s=%u count=%u errors=%d\n", i,
				(int)atomic_get(&channels[i].ready), (int)atomic_get(&channels[i].z),
				(unsigned int)((count - previous[i]) * 1000ULL / (now-last)),
				count, (int)atomic_get(&channels[i].errors));
			previous[i] = count;
		}
		last = now;
	}
	return 0;
}
