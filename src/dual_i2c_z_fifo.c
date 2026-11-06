/*
 * Dual independent I2C, Z-axis acceleration via the ICM20948 hardware FIFO.
 *
 * In single-FIFO configuration the hardware FIFO stores raw sensor packets
 * without a header: 6 bytes per accelerometer sample (X, Y, Z, big-endian).
 * Reading a batch amortizes the per-transaction cost, which is what limited
 * the register-polling version to about 1.2 kHz.
 *
 * Still not a lossless, synchronized recorder: each bus/thread has its own
 * FIFO and clock domain, and only the Z value plus a sample count is kept.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define ICM_ADDR		0x68

/* Bank 0 */
#define REG_USER_CTRL		0x03
#define REG_PWR_MGMT_1		0x06
#define REG_PWR_MGMT_2		0x07
#define REG_FIFO_EN_1		0x66
#define REG_FIFO_EN_2		0x67
#define REG_FIFO_RST		0x68
#define REG_FIFO_MODE		0x69
#define REG_FIFO_COUNT_H	0x70
#define REG_FIFO_COUNT_L	0x71
#define REG_FIFO_R_W		0x72
#define REG_FIFO_CFG		0x76
#define REG_BANK_SEL		0x7f

/* Bank 2 */
#define REG_ACCEL_SMPLRT_DIV_1	0x10
#define REG_ACCEL_SMPLRT_DIV_2	0x11
#define REG_ACCEL_CONFIG	0x14

#define USER_CTRL_FIFO_EN	BIT(6)
#define FIFO_EN_2_ACCEL		BIT(4)
#define FIFO_SAMPLE_BYTES	6
#define FIFO_BATCH_SAMPLES	42	/* 252 bytes, fits the 256-byte EasyDMA limit */
#define FIFO_BATCH_BYTES	(FIFO_BATCH_SAMPLES * FIFO_SAMPLE_BYTES)

struct channel {
	const struct device *bus;
	atomic_t ready, count, errors, z, fifo_max;
	uint8_t buf[FIFO_BATCH_BYTES];
};

static struct channel channels[2] = {
	{ .bus = DEVICE_DT_GET(DT_NODELABEL(i2c0)) },
	{ .bus = DEVICE_DT_GET(DT_NODELABEL(i2c1)) },
};

static int reg_write(struct channel *c, uint8_t reg, uint8_t value)
{
	int ret = i2c_reg_write_byte(c->bus, ICM_ADDR, reg, value);

	if (ret) {
		printk("%s reg=%02x write %02x failed ret=%d\n",
		       c->bus->name, reg, value, ret);
	}
	return ret;
}

static int reg_write_check(struct channel *c, uint8_t reg, uint8_t value)
{
	uint8_t actual = 0;
	int ret = i2c_reg_write_byte(c->bus, ICM_ADDR, reg, value);

	if (!ret) {
		ret = i2c_reg_read_byte(c->bus, ICM_ADDR, reg, &actual);
	}
	if (!ret && actual != value) {
		ret = -EIO;
	}
	if (ret) {
		printk("%s reg=%02x expected=%02x actual=%02x ret=%d\n",
		       c->bus->name, reg, value, actual, ret);
	}
	return ret;
}

static int set_bank(struct channel *c, uint8_t bank)
{
	return reg_write_check(c, REG_BANK_SEL, (uint8_t)(bank << 4));
}

static int fifo_prepare(struct channel *c)
{
	uint8_t ctrl = 0;
	int ret;

	/* Stop the FIFO before changing its configuration. */
	ret = i2c_reg_read_byte(c->bus, ICM_ADDR, REG_USER_CTRL, &ctrl);
	if (ret) {
		return ret;
	}
	ret = reg_write(c, REG_USER_CTRL, ctrl & ~USER_CTRL_FIFO_EN);
	if (ret) {
		return ret;
	}

	/* Single FIFO, no header, all hardware sensor data concatenated. */
	if ((ret = reg_write_check(c, REG_FIFO_CFG, 0x00))) return ret;
	if ((ret = reg_write(c, REG_FIFO_RST, 0x1f))) return ret;
	k_msleep(2);
	if ((ret = reg_write(c, REG_FIFO_RST, 0x00))) return ret;
	if ((ret = reg_write(c, REG_FIFO_EN_1, 0x00))) return ret;
	if ((ret = reg_write(c, REG_FIFO_EN_2, FIFO_EN_2_ACCEL))) return ret;
	if ((ret = reg_write_check(c, REG_FIFO_EN_2, FIFO_EN_2_ACCEL))) return ret;
	if ((ret = reg_write_check(c, REG_FIFO_MODE, 0x00))) return ret;

	ctrl |= USER_CTRL_FIFO_EN;
	if ((ret = reg_write_check(c, REG_USER_CTRL, ctrl))) return ret;

	k_msleep(10);
	return 0;
}

static int setup(struct channel *c)
{
	uint8_t id = 0;
	int ret;

	if (!device_is_ready(c->bus)) {
		return -ENODEV;
	}

	/* Wake, reset, then select the automatic clock source. */
	if ((ret = set_bank(c, 0))) return ret;
	if ((ret = reg_write(c, REG_PWR_MGMT_1, 0x80))) return ret;
	k_msleep(100);
	if ((ret = reg_write_check(c, REG_PWR_MGMT_1, 0x01))) return ret;
	k_msleep(10);

	if ((ret = i2c_reg_read_byte(c->bus, ICM_ADDR, 0x00, &id))) return ret;
	printk("%s WHO_AM_I=0x%02x ret=%d\n", c->bus->name, id, ret);
	if (id != 0xea) return -ENODEV;

	/* Accelerometer on, gyroscope off, no low-power cycling. */
	if ((ret = reg_write_check(c, REG_PWR_MGMT_2, 0x07))) return ret;
	if ((ret = reg_write_check(c, 0x05, 0x00))) return ret;
	if ((ret = reg_write_check(c, 0x03, 0x00))) return ret;

	/* +/-8 g, DLPF bypassed, sample-rate divider zero (nominal 4500 Hz). */
	if ((ret = set_bank(c, 2))) return ret;
	if ((ret = reg_write_check(c, REG_ACCEL_SMPLRT_DIV_1, 0x00))) return ret;
	if ((ret = reg_write_check(c, REG_ACCEL_SMPLRT_DIV_2, 0x00))) return ret;
	if ((ret = reg_write_check(c, REG_ACCEL_CONFIG, 0x04))) return ret;

	if ((ret = set_bank(c, 0))) return ret;
	if ((ret = fifo_prepare(c))) return ret;

	atomic_set(&c->fifo_max, 0);
	return 0;
}

static int fifo_read(struct channel *c)
{
	uint8_t count_bytes[2];
	unsigned int count, bytes, samples;
	int ret;

	ret = i2c_burst_read(c->bus, ICM_ADDR, REG_FIFO_COUNT_H,
			     count_bytes, sizeof(count_bytes));
	if (ret) {
		return ret;
	}

	count = ((unsigned int)(count_bytes[0] & 0x1f) << 8) | count_bytes[1];
	if (count > (unsigned int)atomic_get(&c->fifo_max)) {
		atomic_set(&c->fifo_max, (int)count);
	}

	/* Only consume whole samples; a trailing partial sample stays in FIFO. */
	bytes = count - (count % FIFO_SAMPLE_BYTES);
	if (bytes > FIFO_BATCH_BYTES) {
		bytes = FIFO_BATCH_BYTES;
	}
	if (bytes == 0) {
		return 0;
	}

	ret = i2c_burst_read(c->bus, ICM_ADDR, REG_FIFO_R_W, c->buf, bytes);
	if (ret) {
		return ret;
	}

	samples = bytes / FIFO_SAMPLE_BYTES;
	for (unsigned int i = 0; i < samples; i++) {
		const uint8_t *p = &c->buf[i * FIFO_SAMPLE_BYTES];

		atomic_set(&c->z, (int16_t)(((uint16_t)p[4] << 8) | p[5]));
		atomic_inc(&c->count);
	}
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
		printk("%s FIFO ready: 6 bytes/sample, batch=%d bytes\n",
		       c->bus->name, FIFO_BATCH_BYTES);

		while (true) {
			ret = fifo_read(c);
			if (ret) {
				atomic_inc(&c->errors);
				atomic_set(&c->ready, 0);
				printk("%s fifo read failed %d; reinit\n",
				       c->bus->name, ret);
				k_msleep(100);
				break;
			}
			k_msleep(2);
		}
	}
}

K_THREAD_DEFINE(imu0_thread, 1536, worker, &channels[0], NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(imu1_thread, 1536, worker, &channels[1], NULL, NULL, 5, 0, 0);

int main(void)
{
	uint32_t previous[2] = {0};
	int64_t last = k_uptime_get();
	int fifo_peak[2] = {0};

	printk("DUAL I2C Z FIFO: IMU0 SCL=29 SDA=28; IMU1 SCL=31 SDA=30; 400kHz, addr=0x68\n");
	printk("+/-8g, 4096 LSB/g; nominal ODR=4500Hz; hardware FIFO, NOT synchronized\n");

	while (true) {
		k_msleep(2000);
		int64_t now = k_uptime_get();

		for (int i = 0; i < 2; i++) {
			uint32_t count = (uint32_t)atomic_get(&channels[i].count);
			int peak = (int)atomic_get(&channels[i].fifo_max);

			if (peak > fifo_peak[i]) {
				fifo_peak[i] = peak;
			}
			printk("IMU%d ready=%d z=%d samples/s=%u count=%u errors=%d fifo_max=%d\n",
			       i, (int)atomic_get(&channels[i].ready),
			       (int)atomic_get(&channels[i].z),
			       (unsigned int)((count - previous[i]) * 1000ULL / (now - last)),
			       count, (int)atomic_get(&channels[i].errors), fifo_peak[i]);
			previous[i] = count;
		}
		last = now;
	}
	return 0;
}
