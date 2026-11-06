/*
 * ICM-20948 SPI driver for Zephyr.
 *
 * The driver owns the device configuration (reset, clock source, ranges,
 * DLPF, data-ready interrupt) and exposes a small, high-rate read API.
 * Two devices can share one SPI bus when each has its own chip select and
 * data-ready GPIO.
 *
 * Concurrency:
 *   - icm20948_read()/icm20948_read_wait() may be called from one thread per
 *     device.  Register access is serialized by an internal mutex.
 *   - icm20948_wait_data() only consumes the data-ready semaphore.
 *
 * The two ICM-20948 dies have independent clocks.  The timestamp is the
 * local k_cycle_get_32() value captured in the data-ready interrupt; it can
 * be used to measure each sensor's sample cadence, but it does not make the
 * two sensors sample simultaneously.
 */

#ifndef ICM20948_H_
#define ICM20948_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Expected value of the WHO_AM_I register (bank 0, address 0x00). */
#define ICM20948_WHO_AM_I_VALUE 0xEA

/** One raw sample, in sensor LSBs. */
struct icm20948_sample {
	/** k_cycle_get_32() taken in the data-ready interrupt. */
	uint32_t timestamp;
	/** Data-ready sequence number.  Gaps indicate a missed interrupt. */
	uint32_t sequence;
	/** Raw accelerometer X/Y/Z, big-endian registers converted to host order. */
	int16_t accel[3];
	/** Raw gyroscope X/Y/Z. */
	int16_t gyro[3];
	/** Raw temperature output. */
	int16_t temperature;
};

/** Driver counters.  All fields are monotonically increasing. */
struct icm20948_stats {
	uint32_t irq_count;
	uint32_t sample_count;
	uint32_t overrun_count;
	uint32_t error_count;
};

/**
 * @brief Read one sample immediately.
 *
 * This function does not wait for a data-ready edge.  It is normally called
 * after icm20948_wait_data() or icm20948_read_wait().
 *
 * @return 0 on success, negative errno on failure.
 */
int icm20948_read(const struct device *dev, struct icm20948_sample *sample);

/**
 * @brief Wait for a data-ready edge and read one sample.
 *
 * @param timeout Timeout, for example K_MSEC(10).
 * @return 0 on success, -EAGAIN on timeout, other negative errno on failure.
 */
int icm20948_read_wait(const struct device *dev,
		       struct icm20948_sample *sample,
		       k_timeout_t timeout);

/**
 * @brief Wait for the next data-ready edge without reading the sensor.
 *
 * @return 0 on success, -EAGAIN on timeout.
 */
int icm20948_wait_data(const struct device *dev, k_timeout_t timeout);

/**
 * @brief Read the WHO_AM_I register.
 *
 * This is useful for a production test or a board bring-up check and can be
 * called after icm20948_init() has completed.
 */
int icm20948_get_who_am_i(const struct device *dev, uint8_t *id);

/** @brief Snapshot the driver counters. */
void icm20948_get_stats(const struct device *dev, struct icm20948_stats *stats);

/** @brief Accelerometer sensitivity in LSB/g for the configured range. */
float icm20948_accel_lsb_per_g(const struct device *dev);

/** @brief Gyroscope sensitivity in LSB/(deg/s) for the configured range. */
float icm20948_gyro_lsb_per_dps(const struct device *dev);

/** @brief Convert a raw temperature word to degrees Celsius. */
float icm20948_temp_to_c(int16_t raw);

#ifdef __cplusplus
}
#endif

#endif /* ICM20948_H_ */
