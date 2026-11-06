/*
 * Fixed-frequency MAX98357A test tone.
 *
 * The nRF52832 I2S controller is configured for 8 kHz, 16-bit stereo.  The
 * MAX98357A is one of the few class-D amplifiers that accepts an 8 kHz LRCLK,
 * and 8 kHz is the only rate the nRF52832's 32 MHz clock can generate exactly
 * for the amplifier's supported LRCLK list.
 *
 * A single continuous sine wave is streamed from one thread.  The phase
 * accumulator is kept across blocks so block boundaries do not introduce a
 * discontinuity.  The tone frequency and amplitude are selected at build time
 * through Kconfig (CONFIG_APP_TONE_FREQ_HZ / CONFIG_APP_TONE_AMPLITUDE).
 *
 * Wiring (nRF52 DK pin names):
 *   P0.05 -> MAX98357A BCLK
 *   P0.04 -> MAX98357A LRC / LRCLK
 *   P0.03 -> MAX98357A DIN
 *   P0.07 -> MAX98357A SD / EN
 *   GND   -> MAX98357A GND
 *   VIN   -> dedicated 3.3 V or 5 V supply, not the DK 3.3 V pin
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "icm20948.h"
#include "max98357a.h"

#define AMP_NODE DT_NODELABEL(max98357a)
#define IMU0_NODE DT_NODELABEL(icm20948_0)
#define IMU1_NODE DT_NODELABEL(icm20948_1)

#define TONE_FRAMES 128U
#define TONE_SAMPLE_RATE 8000U

/* A full cycle of sine, scaled to 16-bit signed full scale. */
static const int16_t sine_table[256] = {
	     0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
	  6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
	 12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
	 18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
	 23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
	 27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
	 30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
	 32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
	 32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
	 32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
	 30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
	 27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
	 23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
	 18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
	 12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
	  6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
	     0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
	 -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
	-18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
	-23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
	-30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
	-32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
	-32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
	-30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
	-23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
	-18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
	 -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

static const struct device *const amp_dev = DEVICE_DT_GET(AMP_NODE);

static const uint32_t tone_phase_inc =
	(uint32_t)(((uint64_t)CONFIG_APP_TONE_FREQ_HZ << 32) /
		   TONE_SAMPLE_RATE);

BUILD_ASSERT(CONFIG_APP_TONE_FREQ_HZ >= 20 &&
	     CONFIG_APP_TONE_FREQ_HZ <= 2000,
	     "tone frequency out of supported range");

static void fill_tone(int16_t *frames, size_t frame_count, uint32_t *phase)
{
	for (size_t i = 0; i < frame_count; i++) {
		int16_t sample = sine_table[*phase >> 24];
		int32_t scaled = ((int32_t)sample *
				  CONFIG_APP_TONE_AMPLITUDE) / 32767;

		frames[2 * i] = (int16_t)scaled;
		frames[2 * i + 1] = (int16_t)scaled;
		*phase += tone_phase_inc;
	}
}

static void tone_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	static int16_t frames[TONE_FRAMES * 2];
	uint32_t phase = 0;
	uint32_t consecutive_errors = 0;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	ret = max98357a_start(dev);
	if (ret != 0) {
		printk("AMP: start failed (%d)\n", ret);
		return;
	}

	printk("AMP: playing %d Hz at %u Hz sample rate, amplitude %d\n",
	       CONFIG_APP_TONE_FREQ_HZ, max98357a_sample_rate(dev),
	       CONFIG_APP_TONE_AMPLITUDE);

	while (true) {
		fill_tone(frames, TONE_FRAMES, &phase);

		ret = max98357a_write(dev, frames, TONE_FRAMES);
		if (ret == 0) {
			if (consecutive_errors != 0) {
				printk("AMP: stream recovered after %u errors\n",
				       consecutive_errors);
				consecutive_errors = 0;
			}
			continue;
		}

		consecutive_errors++;

		/* A dropped stream leaves the I2S block unusable.  Stop it
		 * cleanly (SD_MODE first) and restart the clock/amplifier.
		 */
		if (ret == -EIO) {
			int write_ret = ret;

			(void)max98357a_stop(dev);
			k_msleep(5);
			ret = max98357a_start(dev);
			if (ret != 0) {
				printk("AMP: restart failed (%d)\n", ret);
			}
			ret = write_ret;
		}

		if (consecutive_errors == 1 ||
		    (consecutive_errors % 100U) == 0U) {
			printk("AMP: write failed (%d), errors=%u\n", ret,
			       consecutive_errors);
		}

		k_msleep(1);
	}
}

K_THREAD_STACK_DEFINE(tone_stack, 1536);
static struct k_thread tone_thread_desc;

static const struct {
	const struct device *dev;
	unsigned int id;
} imu_dev[] = {
#if DT_NODE_HAS_STATUS(IMU0_NODE, okay)
	{ DEVICE_DT_GET(IMU0_NODE), 0 },
#endif
#if DT_NODE_HAS_STATUS(IMU1_NODE, okay)
	{ DEVICE_DT_GET(IMU1_NODE), 1 },
#endif
};

static void imu_report_once(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(imu_dev); i++) {
		const struct device *dev = imu_dev[i].dev;
		bool ready = device_is_ready(dev);
		uint8_t who_am_i = 0;
		int ret;

		ret = icm20948_get_who_am_i(dev, &who_am_i);
		if (!ready) {
			printk("IMU%u: ready=0 who=0x%02x read_ret=%d\n",
			       imu_dev[i].id, who_am_i, ret);
			continue;
		}
		if (ret != 0 || who_am_i != ICM20948_WHO_AM_I_VALUE) {
			printk("IMU%u: communication lost, who=0x%02x read_ret=%d\n",
			       imu_dev[i].id, who_am_i, ret);
			continue;
		}

		struct icm20948_sample sample;
		struct icm20948_stats stats;

		ret = icm20948_read(dev, &sample);
		if (ret != 0) {
			printk("IMU%u: WHO=0x%02x sample read failed (%d)\n",
			       imu_dev[i].id, who_am_i, ret);
			continue;
		}

		icm20948_get_stats(dev, &stats);
		printk("IMU%u: WHO=0x%02x acc_mg=(%d,%d,%d) "
		       "gyro_mdps=(%d,%d,%d) irq=%u ovr=%u err=%u\n",
		       imu_dev[i].id, who_am_i,
		       sample.accel[0] * 1000 / 4096,
		       sample.accel[1] * 1000 / 4096,
		       sample.accel[2] * 1000 / 4096,
		       sample.gyro[0] * 2000 / 131,
		       sample.gyro[1] * 2000 / 131,
		       sample.gyro[2] * 2000 / 131,
		       stats.irq_count, stats.overrun_count,
		       stats.error_count);
	}
}

int main(void)
{
	printk("\n*** nRF52832 MAX98357A fixed-tone test ***\n");
	printk("Tone: %d Hz, amplitude %d/32767, LRCLK: %u Hz\n",
	       CONFIG_APP_TONE_FREQ_HZ, CONFIG_APP_TONE_AMPLITUDE,
	       TONE_SAMPLE_RATE);
	printk("Pins: BCLK=P0.05 LRCLK=P0.04 DIN=P0.03 SD=P0.07\n");

	if (!device_is_ready(amp_dev)) {
		printk("AMP: device not ready\n");
	} else {
		k_thread_create(&tone_thread_desc, tone_stack,
			K_THREAD_STACK_SIZEOF(tone_stack), tone_thread,
			(void *)amp_dev, NULL, NULL, 6, 0, K_NO_WAIT);
		k_thread_name_set(&tone_thread_desc, "tone");
	}

	printk("IMU: reporting %u enabled device(s) once every 2 s\n",
	       (unsigned int)ARRAY_SIZE(imu_dev));

	while (true) {
		imu_report_once();
		k_msleep(2000);
	}

	return 0;
}
