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

	/* Read OUTX_L_A..OUTZ_H_A (6 bytes, signed 16-bit little-endian). */
	ret = i2c_burst_read(i2c, LSM6DSO_I2C_ADDR,
			     LSM6DSO_REG_OUTX_L_A, buf, sizeof(buf));
	if (ret) {
		LOG_ERR("accel burst read failed: %d", ret);
		test_report("i2c_imu_accel", TEST_FAIL, "burst err %d", ret);
		return;
	}

	int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
	int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
	int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);

	/* Single-precision is plenty for a 16-bit raw count scaled by a
	 * mg/LSB constant; avoids pulling in soft-float double routines. */
	float ax = rx * (float)LSM6DSO_ACCEL_LSB_MS2;
	float ay = ry * (float)LSM6DSO_ACCEL_LSB_MS2;
	float az = rz * (float)LSM6DSO_ACCEL_LSB_MS2;

	/* Magnitude should be ~9.81 m/s² at rest — the board orientation
	 * just shifts which axis carries the gravity vector. */
	float mag = sqrtf(ax * ax + ay * ay + az * az);

	LOG_INF("accel raw: X=%6d Y=%6d Z=%6d", rx, ry, rz);
	LOG_INF("accel m/s2: X=%6.2f Y=%6.2f Z=%6.2f  |g|=%5.2f",
		(double)ax, (double)ay, (double)az, (double)mag);

	/* Sanity: at rest the magnitude should be within ~1 m/s² of 9.81. */
	if (mag > 8.5f && mag < 11.0f) {
		test_report("i2c_imu_accel", TEST_PASS,
			    "X=%.2f Y=%.2f Z=%.2f |g|=%.2f m/s2",
			    (double)ax, (double)ay, (double)az, (double)mag);
	} else {
		test_report("i2c_imu_accel", TEST_FAIL,
			    "|g|=%.2f m/s2 out of [8.5,11.0]", (double)mag);
	}
}
