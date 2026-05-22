#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <math.h>

#include "i2c_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(i2c_probe, LOG_LEVEL_INF);

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c2));

/* LSM6DSO on i2c2 (EVT2). SDO strapped to GND -> 7-bit addr 0x6a.
 * WHO_AM_I register 0x0F should read 0x6C. */
#define LSM6DSO_I2C_ADDR     0x6a
#define LSM6DSO_REG_WHO_AM_I 0x0f
#define LSM6DSO_WHO_AM_I_VAL 0x6c

/* Accelerometer config + output registers. CTRL1_XL=0x40 puts the
 * accel at 104 Hz ODR + ±2 g full-scale. OUTX..OUTZ are signed 16-bit
 * little-endian. At ±2 g the LSB weight is 0.061 mg, so a sample of N
 * counts represents N * 0.061e-3 * 9.80665 m/s². The Z axis should
 * read ~+9.81 m/s² with the board flat and chip side up. */
#define LSM6DSO_REG_CTRL1_XL 0x10
#define LSM6DSO_CTRL1_XL_104HZ_2G 0x40
#define LSM6DSO_REG_OUTX_L_A 0x28
#define LSM6DSO_ACCEL_LSB_MS2  (0.061e-3 * 9.80665)

/* Average over a burst of samples to smooth out per-sample noise (a
 * single reading drifts a few % off 9.81; the mean is much tighter).
 * At 104 Hz ODR the sample period is ~9.6 ms, so 100 samples ≈ 1 s.
 * The last ACCEL_PRINT_LAST raw readings are logged so you can see the
 * live data, not just the mean. */
#define ACCEL_SAMPLE_COUNT     100
#define ACCEL_PRINT_LAST       10
#define ACCEL_SAMPLE_PERIOD_MS 10

bool i2c_probe_ready(void)
{
	if (!device_is_ready(i2c)) {
		LOG_ERR("i2c2 not ready");
		return false;
	}
	return true;
}

void i2c_scan(void)
{
	uint8_t dummy;
	int found = 0;
	bool found_npm1300 = false;

	if (!i2c_probe_ready()) {
		test_report("i2c2", TEST_FAIL, "i2c2 device not ready");
		return;
	}

	LOG_INF("I2C scan on %s:", i2c->name);

	for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
		if (i2c_write(i2c, &dummy, 0, addr) == 0) {
			LOG_INF("  device found at 0x%02x", addr);
			found++;
			if (addr == 0x6b) {
				found_npm1300 = true;
			}
		}
	}

	if (!found) {
		LOG_WRN("  no devices responded");
	}

	test_report("i2c2", found_npm1300 ? TEST_PASS : TEST_FAIL,
		    "%d ACKs total, npm1300@0x6b=%s",
		    found, found_npm1300 ? "yes" : "no");
}

void i2c_imu_whoami(void)
{
	uint8_t val = 0;
	int ret;

	if (!i2c_probe_ready()) {
		test_report("i2c_imu", TEST_FAIL, "i2c2 device not ready");
		return;
	}

	ret = i2c_reg_read_byte(i2c, LSM6DSO_I2C_ADDR,
				LSM6DSO_REG_WHO_AM_I, &val);
	if (ret) {
		LOG_ERR("imu WHO_AM_I read failed: %d (no ACK @ 0x%02x?)",
			ret, LSM6DSO_I2C_ADDR);
		test_report("i2c_imu", TEST_FAIL, "i2c err %d", ret);
		return;
	}

	LOG_INF("imu WHO_AM_I (0x0F) = 0x%02x (expected 0x6C for LSM6DSO)", val);

	if (val == LSM6DSO_WHO_AM_I_VAL) {
		LOG_INF("  -> LSM6DSO detected on i2c2: PASS");
		test_report("i2c_imu", TEST_PASS, "WHO_AM_I=0x%02x", val);
	} else {
		LOG_WRN("  -> unexpected WHO_AM_I: FAIL");
		test_report("i2c_imu", TEST_FAIL,
			    "WHO_AM_I=0x%02x (want 0x6C)", val);
	}
}

void i2c_imu_accel_read(void)
{
	uint8_t buf[6];
	int ret;

	if (!i2c_probe_ready()) {
		test_report("i2c_imu_accel", TEST_FAIL, "i2c2 not ready");
		return;
	}

	/* Wake the accel: ODR = 104 Hz, full-scale = +/-2 g. After this the
	 * sensor needs a couple of ODR periods (~20 ms) before the first
	 * sample is valid; 25 ms is comfortably enough. */
	ret = i2c_reg_write_byte(i2c, LSM6DSO_I2C_ADDR,
				 LSM6DSO_REG_CTRL1_XL,
				 LSM6DSO_CTRL1_XL_104HZ_2G);
	if (ret) {
		LOG_ERR("CTRL1_XL write failed: %d", ret);
		test_report("i2c_imu_accel", TEST_FAIL, "CTRL1_XL err %d", ret);
		return;
	}
	k_msleep(25);

	/* Accumulate raw counts over ACCEL_SAMPLE_COUNT samples. int32 sum
	 * can't overflow: 100 * 32767 ≈ 3.3M << INT32_MAX. Keep the last
	 * ACCEL_PRINT_LAST raw triples for the live readout. */
	int32_t sum_x = 0, sum_y = 0, sum_z = 0;
	int16_t last_x[ACCEL_PRINT_LAST];
	int16_t last_y[ACCEL_PRINT_LAST];
	int16_t last_z[ACCEL_PRINT_LAST];

	for (int i = 0; i < ACCEL_SAMPLE_COUNT; i++) {
		ret = i2c_burst_read(i2c, LSM6DSO_I2C_ADDR,
				     LSM6DSO_REG_OUTX_L_A, buf, sizeof(buf));
		if (ret) {
			LOG_ERR("accel burst read failed at sample %d: %d",
				i, ret);
			test_report("i2c_imu_accel", TEST_FAIL,
				    "burst err %d at sample %d", ret, i);
			return;
		}

		int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
		int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
		int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);

		sum_x += rx;
		sum_y += ry;
		sum_z += rz;

		int slot = i - (ACCEL_SAMPLE_COUNT - ACCEL_PRINT_LAST);
		if (slot >= 0) {
			last_x[slot] = rx;
			last_y[slot] = ry;
			last_z[slot] = rz;
		}

		k_msleep(ACCEL_SAMPLE_PERIOD_MS);
	}

	/* Single-precision throughout — plenty for 16-bit counts scaled by
	 * a mg/LSB constant, and avoids soft-float double routines. */
	const float k = (float)LSM6DSO_ACCEL_LSB_MS2;

	LOG_INF("accel: last %d of %d samples (m/s2):",
		ACCEL_PRINT_LAST, ACCEL_SAMPLE_COUNT);
	for (int i = 0; i < ACCEL_PRINT_LAST; i++) {
		float x = last_x[i] * k;
		float y = last_y[i] * k;
		float z = last_z[i] * k;
		LOG_INF("  [%2d] X=%6.2f Y=%6.2f Z=%6.2f  |g|=%5.2f",
			i + 1, (double)x, (double)y, (double)z,
			(double)sqrtf(x * x + y * y + z * z));
	}

	float ax = (sum_x / (float)ACCEL_SAMPLE_COUNT) * k;
	float ay = (sum_y / (float)ACCEL_SAMPLE_COUNT) * k;
	float az = (sum_z / (float)ACCEL_SAMPLE_COUNT) * k;
	float mag = sqrtf(ax * ax + ay * ay + az * az);

	LOG_INF("accel avg over %d: X=%6.2f Y=%6.2f Z=%6.2f  |g|=%5.2f m/s2",
		ACCEL_SAMPLE_COUNT, (double)ax, (double)ay, (double)az,
		(double)mag);

	/* Sanity: at rest the averaged magnitude should sit close to 9.81. */
	if (mag > 8.5f && mag < 11.0f) {
		test_report("i2c_imu_accel", TEST_PASS,
			    "avg|g|=%.2f m/s2 (X=%.2f Y=%.2f Z=%.2f, n=%d)",
			    (double)mag, (double)ax, (double)ay, (double)az,
			    ACCEL_SAMPLE_COUNT);
	} else {
		test_report("i2c_imu_accel", TEST_FAIL,
			    "avg|g|=%.2f m/s2 out of [8.5,11.0]", (double)mag);
	}
}
