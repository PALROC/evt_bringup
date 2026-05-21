#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "i2c_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(i2c_probe, LOG_LEVEL_INF);

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c2));

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
