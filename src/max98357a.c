/*
 * MAX98357A I2S class-D amplifier driver.
 *
 * MAX98357A only supports LRCLK of 8/16/32/44.1/48/88.2/96 kHz.  On an
 * nRF52832 the internal 32 MHz clock can produce 8 kHz exactly (MCK =
 * 32 MHz / 125 = 256 kHz, MCK/LRCK = 32), so 8 kHz is used here.  At 8 kHz
 * the audio Nyquist frequency is 4 kHz, which covers the 50-800 Hz excitation
 * band used by this project.
 */

#include "max98357a.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(max98357a, LOG_LEVEL_INF);

#define DT_DRV_COMPAT maxim_max98357a

#define MAX98357A_BLOCK_BYTES	512U
#define MAX98357A_BLOCK_FRAMES	(MAX98357A_BLOCK_BYTES / (2U * sizeof(int16_t)))
#define MAX98357A_SLAB_BLOCKS	4U

K_MEM_SLAB_DEFINE_STATIC(max98357a_tx_slab, MAX98357A_BLOCK_BYTES,
			 MAX98357A_SLAB_BLOCKS, 4);

struct max98357a_config {
	const struct device *i2s;
	struct gpio_dt_spec sdin;
	uint32_t sample_rate;
	uint8_t word_size;
	uint8_t preload_blocks;
};

struct max98357a_data {
	bool running;
	atomic_t write_count;
	atomic_t error_count;
};

static int max98357a_configure_i2s(const struct device *dev)
{
	const struct max98357a_config *cfg = dev->config;
	struct i2s_config i2s_cfg = {
		.word_size = cfg->word_size,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER |
			   I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = cfg->sample_rate,
		.mem_slab = &max98357a_tx_slab,
		.block_size = MAX98357A_BLOCK_BYTES,
		.timeout = 1000,
	};
	int ret;

	ret = i2s_configure(cfg->i2s, I2S_DIR_TX, &i2s_cfg);
	if (ret != 0) {
		LOG_ERR("i2s_configure failed: %d", ret);
	}

	return ret;
}

static int max98357a_init(const struct device *dev)
{
	const struct max98357a_config *cfg = dev->config;
	struct max98357a_data *data = dev->data;
	int ret;

	data->running = false;
	atomic_set(&data->write_count, 0);
	atomic_set(&data->error_count, 0);

	if (!device_is_ready(cfg->i2s)) {
		LOG_ERR("I2S controller not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&cfg->sdin)) {
		LOG_ERR("SD_MODE GPIO not ready");
		return -ENODEV;
	}

	/* SD_MODE low: amplifier in shutdown while the system boots. */
	ret = gpio_pin_configure_dt(&cfg->sdin, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("SD_MODE GPIO configure failed: %d", ret);
		return ret;
	}

	LOG_INF("MAX98357A ready: %u Hz, %u-bit, %u frames/block",
		cfg->sample_rate, cfg->word_size, MAX98357A_BLOCK_FRAMES);

	return 0;
}

int max98357a_start(const struct device *dev)
{
	const struct max98357a_config *cfg = dev->config;
	struct max98357a_data *data = dev->data;
	static const uint8_t silence[MAX98357A_BLOCK_BYTES];
	int ret;

	if (data->running) {
		return 0;
	}

	ret = max98357a_configure_i2s(dev);
	if (ret != 0) {
		return ret;
	}

	/* Prime the DMA queue with silence.  This also guarantees that BCLK
	 * and LRCLK are stable before SD_MODE is raised.
	 */
	for (uint8_t i = 0; i < cfg->preload_blocks; i++) {
		ret = i2s_buf_write(cfg->i2s, (void *)silence,
				    sizeof(silence));
		if (ret != 0) {
			LOG_ERR("silence preload failed: %d", ret);
			goto fail;
		}
	}

	ret = i2s_trigger(cfg->i2s, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret != 0) {
		LOG_ERR("i2s_trigger start failed: %d", ret);
		goto fail;
	}

	/* Let the clocks run for a moment before waking the amplifier. */
	k_msleep(1);

	ret = gpio_pin_set_dt(&cfg->sdin, 1);
	if (ret != 0) {
		LOG_ERR("SD_MODE enable failed: %d", ret);
		(void)i2s_trigger(cfg->i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
		goto fail;
	}

	data->running = true;
	return 0;

fail:
	data->running = false;
	(void)gpio_pin_set_dt(&cfg->sdin, 0);
	return ret;
}

int max98357a_stop(const struct device *dev)
{
	const struct max98357a_config *cfg = dev->config;
	struct max98357a_data *data = dev->data;
	int ret = 0;

	/* Disable the amplifier first; never leave the class-D stage driving
	 * while BCLK/LRCLK disappear.
	 */
	if (gpio_pin_set_dt(&cfg->sdin, 0) != 0) {
		ret = -EIO;
	}

	k_msleep(1);

	if (data->running) {
		int trigger_ret = i2s_trigger(cfg->i2s, I2S_DIR_TX,
					      I2S_TRIGGER_DROP);

		if (trigger_ret != 0 && trigger_ret != -EIO) {
			ret = trigger_ret;
		}
	}

	data->running = false;
	return ret;
}

int max98357a_write(const struct device *dev, const int16_t *frames,
		    size_t frame_count)
{
	const struct max98357a_config *cfg = dev->config;
	struct max98357a_data *data = dev->data;
	size_t bytes;
	int ret;

	if (frames == NULL || frame_count == 0) {
		return -EINVAL;
	}

	bytes = frame_count * 2U * sizeof(int16_t);
	if (bytes > MAX98357A_BLOCK_BYTES) {
		return -EINVAL;
	}

	if (!data->running) {
		return -EIO;
	}

	ret = i2s_buf_write(cfg->i2s, (void *)frames, bytes);
	if (ret != 0) {
		atomic_inc(&data->error_count);
		return ret;
	}

	atomic_inc(&data->write_count);
	return 0;
}

uint32_t max98357a_sample_rate(const struct device *dev)
{
	const struct max98357a_config *cfg = dev->config;

	return cfg->sample_rate;
}

size_t max98357a_max_frames(const struct device *dev)
{
	ARG_UNUSED(dev);

	return MAX98357A_BLOCK_FRAMES;
}

int max98357a_enable(const struct device *dev, bool enable)
{
	const struct max98357a_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->sdin, enable ? 1 : 0);
}

#define MAX98357A_INIT(n)                                                      \
	BUILD_ASSERT(DT_INST_PROP_OR(n, word_size, 16) == 16,                 \
		     "MAX98357A driver supports 16-bit PCM only");             \
	BUILD_ASSERT(DT_INST_PROP_OR(n, sample_rate, 8000) == 8000,           \
		     "nRF52832 can only generate 8 kHz exactly for the "       \
		     "MAX98357A supported LRCLK rates");                       \
	static struct max98357a_data max98357a_data_##n;                       \
	static const struct max98357a_config max98357a_config_##n = {          \
		.i2s = DEVICE_DT_GET(DT_INST_PHANDLE(n, i2s_controller)),       \
		.sdin = GPIO_DT_SPEC_INST_GET(n, sdin_gpios),                   \
		.sample_rate = DT_INST_PROP_OR(n, sample_rate, 8000),           \
		.word_size = DT_INST_PROP_OR(n, word_size, 16),                 \
		.preload_blocks = DT_INST_PROP_OR(n, preload_blocks, 3),        \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(n, max98357a_init, NULL, &max98357a_data_##n,   \
			      &max98357a_config_##n, POST_KERNEL, 85, NULL);

DT_INST_FOREACH_STATUS_OKAY(MAX98357A_INIT)
