#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "i2c_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(i2c_probe, LOG_LEVEL_INF);

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c2));

/* LSM6DSO on i2c2 (EVT2). SDO strapped to GND -> 7-bit addr 0x6a.
 * WHO_AM_I register 0x0F should read 0x6C. */
#define LSM6DSO_I2C_ADDR     0x6a
#define LSM6DSO_REG_WHO_AM_I 0x0f
#define LSM6DSO_WHO_AM_I_VAL 0x6c

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
