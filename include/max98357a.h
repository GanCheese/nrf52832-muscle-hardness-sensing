/*
 * MAX98357A I2S Class-D amplifier driver for Zephyr.
 *
 * The MAX98357A has no register interface.  This driver wraps the I2S
 * controller and the SD_MODE GPIO with the correct ordering:
 *   start: configure I2S -> preload silence -> start clocks -> enable amp
 *   stop : disable amp first -> drop I2S stream (never remove LRCLK while
 *          BCLK is still running)
 *
 * Sample format is signed 16-bit, stereo interleaved (left, right).  The
 * MAX98357A in SD_MODE=high uses the left channel, so write the desired mono
 * waveform to both channels.
 */

#ifndef MAX98357A_H_
#define MAX98357A_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the I2S clocks and take the amplifier out of shutdown.
 *
 * A few blocks of silence are queued first so the amplifier does not see a
 * clock that disappears immediately.  The caller should start writing audio
 * blocks right after this function returns.
 *
 * @return 0 on success, negative errno on failure.
 */
int max98357a_start(const struct device *dev);

/**
 * @brief Stop the stream.
 *
 * SD_MODE is driven low first, then the I2S stream is dropped.  Calling this
 * function when the stream is already stopped is harmless.
 */
int max98357a_stop(const struct device *dev);

/**
 * @brief Queue one block of stereo 16-bit PCM.
 *
 * The data is copied into a driver-owned I2S block before the function
 * returns, so the caller may reuse the buffer immediately.  The function
 * blocks while the TX queue is full (bounded by the I2S timeout).
 *
 * @param frames  Number of stereo frames.  frames * 4 bytes must not exceed
 *                the driver block size (512 bytes, i.e. 128 frames).
 *
 * @return 0 on success, -EINVAL for an invalid size, -EIO if not started.
 */
int max98357a_write(const struct device *dev, const int16_t *frames,
		    size_t frame_count);

/** @brief Configured LRCLK / sample rate in Hz. */
uint32_t max98357a_sample_rate(const struct device *dev);

/** @brief Number of frames that fit in one block. */
size_t max98357a_max_frames(const struct device *dev);

/** @brief Direct control of SD_MODE, mainly for bring-up tests. */
int max98357a_enable(const struct device *dev, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* MAX98357A_H_ */
