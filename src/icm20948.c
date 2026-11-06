/*
 * ICM-20948 SPI driver.
 *
 * Register map reference: ICM-20948 datasheet DS-000189 rev 1.3.
 * SPI mode 0, MSB first, address bit 7 = 1 for read, 0 for write.
 */

#include "icm20948.h"

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(icm20948, LOG_LEVEL_INF);

#define DT_DRV_COMPAT invensense_icm20948

/* User bank 0 */
#define ICM_REG_WHO_AM_I		0x00
#define ICM_REG_USER_CTRL		0x03
#define ICM_REG_LP_CONFIG		0x05
#define ICM_REG_PWR_MGMT_1		0x06
#define ICM_REG_PWR_MGMT_2		0x07
#define ICM_REG_INT_PIN_CFG		0x0F
#define ICM_REG_INT_ENABLE		0x10
#define ICM_REG_INT_ENABLE_1		0x11
#define ICM_REG_INT_ENABLE_2		0x12
#define ICM_REG_INT_ENABLE_3		0x13
#define ICM_REG_INT_STATUS_1		0x1A
#define ICM_REG_ACCEL_XOUT_H		0x2D
#define ICM_REG_FIFO_EN_1		0x66
#define ICM_REG_FIFO_EN_2		0x67
#define ICM_REG_FIFO_RST		0x68
#define ICM_REG_FIFO_MODE		0x69
#define ICM_REG_REG_BANK_SEL		0x7F

/* User bank 2 */
#define ICM_REG_GYRO_SMPLRT_DIV		0x00
#define ICM_REG_GYRO_CONFIG_1		0x01
#define ICM_REG_ODR_ALIGN_EN		0x09
#define ICM_REG_ACCEL_SMPLRT_DIV_1	0x10
#define ICM_REG_ACCEL_SMPLRT_DIV_2	0x11
#define ICM_REG_ACCEL_CONFIG		0x14

/* PWR_MGMT_1 */
#define ICM_PWR_DEVICE_RESET		BIT(7)
#define ICM_PWR_CLKSEL_AUTO		0x01

/* USER_CTRL: keep the shared serial pins in SPI-only mode. */
#define ICM_USER_I2C_IF_DIS		BIT(4)

/* INT_PIN_CFG */
#define ICM_INT_ANYRD_2CLEAR		BIT(4)
#define ICM_INT1_LATCH_EN		BIT(5)
#define ICM_INT1_OPEN			BIT(6)
#define ICM_INT1_ACTL			BIT(7)

/* INT_ENABLE_1 */
#define ICM_INT_RAW_DATA_0_RDY_EN	BIT(0)

/* GYRO_CONFIG_1 / ACCEL_CONFIG */
#define ICM_FCHOICE_DLPF_ENABLED	BIT(0)

#define ICM_ACCEL_FS_SEL(g)	((g) == 2 ? 0 : (g) == 4 ? 1 : (g) == 8 ? 2 : 3)
#define ICM_GYRO_FS_SEL(g)	((g) == 250 ? 0 : (g) == 500 ? 1 : (g) == 1000 ? 2 : 3)

#define ICM_BURST_ACCEL_GYRO_TEMP	14
#define ICM_SPI_BUF_SIZE		(ICM_BURST_ACCEL_GYRO_TEMP + 1)

struct icm20948_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec int_gpio;
	uint16_t accel_range_g;
	uint16_t gyro_range_dps;
	uint8_t accel_fs_sel;
	uint8_t gyro_fs_sel;
	uint8_t accel_dlpf_cfg;
	uint8_t gyro_dlpf_cfg;
	bool accel_dlpf_bypass;
	bool gyro_dlpf_bypass;
	uint8_t gyro_sample_rate_div;
};

struct icm20948_data {
	struct k_mutex lock;
	struct k_sem data_ready;
	struct gpio_callback int_cb;

	uint8_t bank;

	atomic_t irq_count;
	atomic_t sample_count;
	atomic_t overrun_count;
	atomic_t error_count;

	uint32_t last_irq_cycle;
	uint32_t sequence;
};

static int icm20948_write_reg(const struct device *dev, uint8_t bank,
			      uint8_t reg, uint8_t value);
static int icm20948_read_regs(const struct device *dev, uint8_t bank,
			      uint8_t reg, uint8_t *buf, size_t len);

static bool icm20948_int_is_active_low(const struct icm20948_config *cfg)
{
	return (cfg->int_gpio.dt_flags & GPIO_ACTIVE_LOW) != 0;
}

static int icm20948_set_bank_locked(const struct device *dev, uint8_t bank)
{
	const struct icm20948_config *cfg = dev->config;
	struct icm20948_data *data = dev->data;
	uint8_t tx[2] = { ICM_REG_REG_BANK_SEL, (uint8_t)(bank << 4) };
	struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	int ret;

	/* Always write the bank select for bring-up reliability.  A cached bank
	 * value can hide a missed bank-select write on a marginal SPI link.
	 */
	ret = spi_write_dt(&cfg->spi, &tx_set);
	if (ret == 0) {
		data->bank = bank;
	}
	/* Explicit CS-high interval between bank selection and register access. */
	k_busy_wait(10);

	return ret;
}

static int icm20948_write_reg_locked(const struct device *dev, uint8_t bank,
				     uint8_t reg, uint8_t value)
{
	const struct icm20948_config *cfg = dev->config;
	uint8_t tx[2] = { reg & 0x7F, value };
	struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	int ret;

	ret = icm20948_set_bank_locked(dev, bank);
	if (ret != 0) {
		return ret;
	}

	return spi_write_dt(&cfg->spi, &tx_set);
}

static int icm20948_write_reg(const struct device *dev, uint8_t bank,
			      uint8_t reg, uint8_t value)
{
	struct icm20948_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = icm20948_write_reg_locked(dev, bank, reg, value);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int icm20948_read_regs_locked(const struct device *dev, uint8_t bank,
				     uint8_t reg, uint8_t *buf, size_t len)
{
	const struct icm20948_config *cfg = dev->config;
	uint8_t tx[ICM_SPI_BUF_SIZE] = { 0 };
	uint8_t rx[ICM_SPI_BUF_SIZE] = { 0 };
	struct spi_buf tx_buf = { .buf = tx, .len = len + 1U };
	struct spi_buf rx_buf = { .buf = rx, .len = len + 1U };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
	int ret;

	if (len + 1U > sizeof(tx)) {
		return -EINVAL;
	}

	ret = icm20948_set_bank_locked(dev, bank);
	if (ret != 0) {
		return ret;
	}

	tx[0] = reg | 0x80;
	ret = spi_transceive_dt(&cfg->spi, &tx_set, &rx_set);
	if (ret != 0) {
		return ret;
	}

	memcpy(buf, &rx[1], len);
	return 0;
}

static int icm20948_read_regs(const struct device *dev, uint8_t bank,
			      uint8_t reg, uint8_t *buf, size_t len)
{
	struct icm20948_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = icm20948_read_regs_locked(dev, bank, reg, buf, len);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int icm20948_read_reg(const struct device *dev, uint8_t bank,
			     uint8_t reg, uint8_t *value)
{
	return icm20948_read_regs(dev, bank, reg, value, 1);
}

static int icm20948_check_reg(const struct device *dev, uint8_t bank,
			      uint8_t reg, uint8_t expected)
{
	uint8_t value;
	int ret = icm20948_read_reg(dev, bank, reg, &value);

	if (ret != 0) {
		LOG_ERR("%s: bank %u reg 0x%02x read failed: %d",
			dev->name, bank, reg, ret);
		return ret;
	}
	if (value != expected) {
		LOG_ERR("%s: bank %u reg 0x%02x = 0x%02x, expected 0x%02x",
			dev->name, bank, reg, value, expected);
		return -EIO;
	}

	return 0;
}

static void icm20948_gpio_isr(const struct device *port,
			      struct gpio_callback *cb,
			      uint32_t pins)
{
	struct icm20948_data *data =
		CONTAINER_OF(cb, struct icm20948_data, int_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	data->last_irq_cycle = k_cycle_get_32();
	data->sequence++;
	atomic_inc(&data->irq_count);

	if (k_sem_count_get(&data->data_ready) > 0) {
		atomic_inc(&data->overrun_count);
	}

	k_sem_give(&data->data_ready);
}

static int icm20948_configure_sensor(const struct device *dev)
{
	const struct icm20948_config *cfg = dev->config;
	uint8_t accel_cfg;
	uint8_t gyro_cfg;
	int ret;

	/* Wake the device and select the automatic best clock. */
	ret = icm20948_write_reg(dev, 0, ICM_REG_PWR_MGMT_1, ICM_PWR_CLKSEL_AUTO);
	if (ret != 0) {
		return ret;
	}
	k_msleep(10);
	ret = icm20948_check_reg(dev, 0, ICM_REG_PWR_MGMT_1, ICM_PWR_CLKSEL_AUTO);
	if (ret != 0) {
		return ret;
	}

	/* Enable all accelerometer and gyroscope axes (low-noise mode). */
	ret = icm20948_write_reg(dev, 0, ICM_REG_PWR_MGMT_2, 0x00);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 0, ICM_REG_LP_CONFIG, 0x00);
	if (ret != 0) {
		return ret;
	}

	/* No FIFO, DMP or I2C master; do not re-enable the I2C slave. */
	ret = icm20948_write_reg(dev, 0, ICM_REG_USER_CTRL, ICM_USER_I2C_IF_DIS);
	if (ret != 0) {
		return ret;
	}

	accel_cfg = (uint8_t)((cfg->accel_dlpf_cfg << 3) |
			      (cfg->accel_fs_sel << 1) |
			      (cfg->accel_dlpf_bypass ? 0 : ICM_FCHOICE_DLPF_ENABLED));
	gyro_cfg = (uint8_t)((cfg->gyro_dlpf_cfg << 3) |
			     (cfg->gyro_fs_sel << 1) |
			     (cfg->gyro_dlpf_bypass ? 0 : ICM_FCHOICE_DLPF_ENABLED));

	ret = icm20948_write_reg(dev, 2, ICM_REG_ODR_ALIGN_EN, 0x01);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 2, ICM_REG_GYRO_SMPLRT_DIV,
				 cfg->gyro_sample_rate_div);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 2, ICM_REG_ACCEL_SMPLRT_DIV_1, 0x00);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 2, ICM_REG_ACCEL_SMPLRT_DIV_2, 0x00);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 2, ICM_REG_GYRO_CONFIG_1, gyro_cfg);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 2, ICM_REG_ACCEL_CONFIG, accel_cfg);
	if (ret != 0) {
		return ret;
	}

	/* SPI writes have no slave acknowledgement.  Verify both bank selection
	 * and configuration instead of treating a completed transfer as success.
	 */
	ret = icm20948_check_reg(dev, 2, ICM_REG_GYRO_CONFIG_1, gyro_cfg);
	if (ret != 0) {
		return ret;
	}
	ret = icm20948_check_reg(dev, 2, ICM_REG_ACCEL_CONFIG, accel_cfg);
	if (ret != 0) {
		return ret;
	}
	ret = icm20948_check_reg(dev, 0, ICM_REG_USER_CTRL, ICM_USER_I2C_IF_DIS);
	if (ret != 0) {
		return ret;
	}
	ret = icm20948_check_reg(dev, 0, ICM_REG_PWR_MGMT_1, ICM_PWR_CLKSEL_AUTO);
	if (ret != 0) {
		return ret;
	}
	ret = icm20948_check_reg(dev, 0, ICM_REG_PWR_MGMT_2, 0x00);
	if (ret != 0) {
		return ret;
	}

	/* Data-ready interrupt: active low/open drain for a button-pin input,
	 * or active high/push-pull otherwise.  Latch until any register read.
	 */
	uint8_t int_pin_cfg = ICM_INT_ANYRD_2CLEAR | ICM_INT1_LATCH_EN;

	if (icm20948_int_is_active_low(cfg)) {
		int_pin_cfg |= ICM_INT1_ACTL | ICM_INT1_OPEN;
	}

	ret = icm20948_write_reg(dev, 0, ICM_REG_INT_PIN_CFG, int_pin_cfg);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 0, ICM_REG_INT_ENABLE, 0x00);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 0, ICM_REG_INT_ENABLE_2, 0x00);
	if (ret != 0) {
		return ret;
	}

	ret = icm20948_write_reg(dev, 0, ICM_REG_INT_ENABLE_3, 0x00);
	if (ret != 0) {
		return ret;
	}

	/* Keep the data-ready interrupt masked until the GPIO is ready. */
	return icm20948_write_reg(dev, 0, ICM_REG_INT_ENABLE_1, 0x00);
}

static int icm20948_init(const struct device *dev)
{
	const struct icm20948_config *cfg = dev->config;
	struct icm20948_data *data = dev->data;
	uint8_t who_am_i = 0;
	int ret;

	k_mutex_init(&data->lock);
	k_sem_init(&data->data_ready, 0, 1);
	atomic_set(&data->irq_count, 0);
	atomic_set(&data->sample_count, 0);
	atomic_set(&data->overrun_count, 0);
	atomic_set(&data->error_count, 0);
	data->last_irq_cycle = 0;
	data->sequence = 0;
	data->bank = 0xFF;

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("%s: SPI bus not ready", dev->name);
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->int_gpio)) {
		LOG_ERR("%s: INT GPIO not ready", dev->name);
		return -ENODEV;
	}

	/* Register access is only guaranteed 100 ms after sensor power-up.
	 * The SPI controller has already configured both chip selects inactive.
	 */
	k_msleep(100);

	/* Soft reset.  The device comes out of reset in bank 0 and sleep mode. */
	ret = icm20948_write_reg(dev, 0, ICM_REG_PWR_MGMT_1, ICM_PWR_DEVICE_RESET);
	if (ret != 0) {
		LOG_ERR("%s: reset failed: %d", dev->name, ret);
		return ret;
	}
	k_msleep(100);
	data->bank = 0;
	LOG_INF("%s: SPI %u Hz, operation=0x%x", dev->name,
		cfg->spi.config.frequency, (unsigned int)cfg->spi.config.operation);

	/* Reset clears USER_CTRL.  Disable the I2C slave before any further
	 * traffic on the shared SPI bus can be mistaken for I2C activity.
	 */
	ret = icm20948_write_reg(dev, 0, ICM_REG_USER_CTRL, ICM_USER_I2C_IF_DIS);
	if (ret != 0) {
		LOG_ERR("%s: SPI-only mode failed: %d", dev->name, ret);
		return ret;
	}

	ret = icm20948_read_reg(dev, 0, ICM_REG_WHO_AM_I, &who_am_i);
	if (ret != 0) {
		LOG_ERR("%s: WHO_AM_I read failed: %d", dev->name, ret);
		return ret;
	}

	if (who_am_i != ICM20948_WHO_AM_I_VALUE) {
		LOG_ERR("%s: unexpected WHO_AM_I 0x%02x (expected 0x%02x)",
			dev->name, who_am_i, ICM20948_WHO_AM_I_VALUE);
		return -ENODEV;
	}
	LOG_INF("%s: post-reset WHO_AM_I=0x%02x", dev->name, who_am_i);
	ret = icm20948_check_reg(dev, 0, ICM_REG_USER_CTRL, ICM_USER_I2C_IF_DIS);
	if (ret != 0) {
		return ret;
	}
	/* Repeated identity reads expose intermittent transfers before setup. */
	for (int i = 0; i < 8; i++) {
		ret = icm20948_check_reg(dev, 0, ICM_REG_WHO_AM_I,
					 ICM20948_WHO_AM_I_VALUE);
		if (ret != 0) {
			LOG_ERR("%s: identity stability check %d failed", dev->name, i);
			return ret;
		}
		k_msleep(1);
	}

	ret = icm20948_configure_sensor(dev);
	if (ret != 0) {
		LOG_ERR("%s: sensor configuration failed: %d", dev->name, ret);
		return ret;
	}

	/* Allow gyro start-up, then check that communication survived setup. */
	k_msleep(50);
	ret = icm20948_check_reg(dev, 0, ICM_REG_WHO_AM_I,
				 ICM20948_WHO_AM_I_VALUE);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("%s: INT GPIO configure failed: %d", dev->name, ret);
		return ret;
	}

	gpio_init_callback(&data->int_cb, icm20948_gpio_isr,
			   BIT(cfg->int_gpio.pin));
	ret = gpio_add_callback(cfg->int_gpio.port, &data->int_cb);
	if (ret != 0) {
		LOG_ERR("%s: INT GPIO callback add failed: %d", dev->name, ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(
		&cfg->int_gpio,
		icm20948_int_is_active_low(cfg) ? GPIO_INT_EDGE_FALLING
					       : GPIO_INT_EDGE_RISING);
	if (ret != 0) {
		LOG_ERR("%s: INT GPIO interrupt configure failed: %d", dev->name, ret);
		gpio_remove_callback(cfg->int_gpio.port, &data->int_cb);
		return ret;
	}

	/* Unmask the data-ready interrupt last so the first edge is not lost. */
	ret = icm20948_write_reg(dev, 0, ICM_REG_INT_ENABLE_1,
				 ICM_INT_RAW_DATA_0_RDY_EN);
	if (ret == 0) {
		ret = icm20948_check_reg(dev, 0, ICM_REG_INT_ENABLE_1,
					 ICM_INT_RAW_DATA_0_RDY_EN);
	}
	if (ret != 0) {
		LOG_ERR("%s: data-ready enable failed: %d", dev->name, ret);
		gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_DISABLE);
		gpio_remove_callback(cfg->int_gpio.port, &data->int_cb);
		return ret;
	}

	LOG_INF("%s: ICM-20948 ready: accel=+/-%u g, gyro=+/-%u dps, DLPF %s/%s",
		dev->name, cfg->accel_range_g, cfg->gyro_range_dps,
		cfg->accel_dlpf_bypass ? "bypass" : "on",
		cfg->gyro_dlpf_bypass ? "bypass" : "on");

	return 0;
}

int icm20948_read(const struct device *dev, struct icm20948_sample *sample)
{
	struct icm20948_data *data = dev->data;
	uint8_t raw[ICM_BURST_ACCEL_GYRO_TEMP];
	uint32_t timestamp;
	uint32_t sequence;
	int ret;

	if (sample == NULL) {
		return -EINVAL;
	}

	timestamp = data->last_irq_cycle;
	sequence = data->sequence;

	ret = icm20948_read_regs(dev, 0, ICM_REG_ACCEL_XOUT_H, raw,
				 sizeof(raw));
	if (ret != 0) {
		atomic_inc(&data->error_count);
		return ret;
	}

	sample->timestamp = timestamp;
	sample->sequence = sequence;

	sample->accel[0] = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
	sample->accel[1] = (int16_t)(((uint16_t)raw[2] << 8) | raw[3]);
	sample->accel[2] = (int16_t)(((uint16_t)raw[4] << 8) | raw[5]);
	sample->gyro[0] = (int16_t)(((uint16_t)raw[6] << 8) | raw[7]);
	sample->gyro[1] = (int16_t)(((uint16_t)raw[8] << 8) | raw[9]);
	sample->gyro[2] = (int16_t)(((uint16_t)raw[10] << 8) | raw[11]);
	sample->temperature = (int16_t)(((uint16_t)raw[12] << 8) | raw[13]);

	atomic_inc(&data->sample_count);
	return 0;
}

int icm20948_read_wait(const struct device *dev,
		       struct icm20948_sample *sample,
		       k_timeout_t timeout)
{
	struct icm20948_data *data = dev->data;
	int ret;

	ret = k_sem_take(&data->data_ready, timeout);
	if (ret != 0) {
		return -EAGAIN;
	}

	return icm20948_read(dev, sample);
}

int icm20948_wait_data(const struct device *dev, k_timeout_t timeout)
{
	struct icm20948_data *data = dev->data;

	return k_sem_take(&data->data_ready, timeout) == 0 ? 0 : -EAGAIN;
}

int icm20948_get_who_am_i(const struct device *dev, uint8_t *id)
{
	if (id == NULL) {
		return -EINVAL;
	}

	return icm20948_read_reg(dev, 0, ICM_REG_WHO_AM_I, id);
}

void icm20948_get_stats(const struct device *dev, struct icm20948_stats *stats)
{
	struct icm20948_data *data = dev->data;

	if (stats == NULL) {
		return;
	}

	stats->irq_count = (uint32_t)atomic_get(&data->irq_count);
	stats->sample_count = (uint32_t)atomic_get(&data->sample_count);
	stats->overrun_count = (uint32_t)atomic_get(&data->overrun_count);
	stats->error_count = (uint32_t)atomic_get(&data->error_count);
}

float icm20948_accel_lsb_per_g(const struct device *dev)
{
	const struct icm20948_config *cfg = dev->config;

	switch (cfg->accel_fs_sel) {
	case 0:
		return 16384.0f; /* +/-2 g */
	case 1:
		return 8192.0f;  /* +/-4 g */
	case 3:
		return 2048.0f;  /* +/-16 g */
	case 2:
	default:
		return 4096.0f;  /* +/-8 g */
	}
}

float icm20948_gyro_lsb_per_dps(const struct device *dev)
{
	const struct icm20948_config *cfg = dev->config;

	switch (cfg->gyro_fs_sel) {
	case 0:
		return 131.0f;  /* +/-250 dps */
	case 2:
		return 32.8f;   /* +/-1000 dps */
	case 3:
		return 16.4f;   /* +/-2000 dps */
	case 1:
	default:
		return 65.5f;   /* +/-500 dps */
	}
}

float icm20948_temp_to_c(int16_t raw)
{
	return (((float)raw - 21.0f) / 333.87f) + 21.0f;
}

#define ICM20948_INIT(n)                                                       \
	static struct icm20948_data icm20948_data_##n;                         \
	static const struct icm20948_config icm20948_config_##n = {            \
		.spi = SPI_DT_SPEC_INST_GET(                                   \
			n, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |              \
				   SPI_TRANSFER_MSB),                          \
		.int_gpio = GPIO_DT_SPEC_INST_GET(n, int_gpios),               \
		.accel_range_g = DT_INST_PROP_OR(n, accel_range_g, 8),          \
		.gyro_range_dps = DT_INST_PROP_OR(n, gyro_range_dps, 500),      \
		.accel_fs_sel = ICM_ACCEL_FS_SEL(                               \
			DT_INST_PROP_OR(n, accel_range_g, 8)),                  \
		.gyro_fs_sel = ICM_GYRO_FS_SEL(                                \
			DT_INST_PROP_OR(n, gyro_range_dps, 500)),               \
		.accel_dlpf_cfg = DT_INST_PROP_OR(n, accel_dlpf_cfg, 7),        \
		.gyro_dlpf_cfg = DT_INST_PROP_OR(n, gyro_dlpf_cfg, 7),          \
		.accel_dlpf_bypass =                                            \
			DT_INST_PROP_OR(n, accel_dlpf_bypass, 0),               \
		.gyro_dlpf_bypass =                                             \
			DT_INST_PROP_OR(n, gyro_dlpf_bypass, 0),                \
		.gyro_sample_rate_div =                                         \
			DT_INST_PROP_OR(n, gyro_sample_rate_div, 0),            \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(n, icm20948_init, NULL, &icm20948_data_##n,      \
			      &icm20948_config_##n, POST_KERNEL, 90, NULL);

DT_INST_FOREACH_STATUS_OKAY(ICM20948_INIT)
