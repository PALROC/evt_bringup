#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "npm1300.h"
#include "test_report.h"

LOG_MODULE_REGISTER(npm1300, LOG_LEVEL_INF);

static const struct device *const charger =
	DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));

static void log_channel(const char *name, enum sensor_channel ch,
			const char *unit)
{
	struct sensor_value v;
	int err = sensor_channel_get(charger, ch, &v);

	if (err) {
		LOG_WRN("  %-7s: read err %d", name, err);
		return;
	}
	LOG_INF("  %-7s: %d.%06d %s", name, v.val1, v.val2, unit);
}

void npm1300_probe(void)
{
	if (!device_is_ready(charger)) {
		LOG_ERR("npm1300 charger device not ready (driver bound? dts wired?)");
		test_report("npm1300", TEST_FAIL, "charger device not ready");
		return;
	}

	int err = sensor_sample_fetch(charger);

	if (err) {
		LOG_ERR("sensor_sample_fetch failed: %d (PMIC unresponsive on i2c?)",
			err);
		test_report("npm1300", TEST_FAIL, "sample_fetch err %d", err);
		return;
	}

	LOG_INF("nPM1300 charger sample:");
	log_channel("VBAT", SENSOR_CHAN_GAUGE_VOLTAGE, "V");
	log_channel("Tntc", SENSOR_CHAN_GAUGE_TEMP, "C");
	log_channel("IBAT", SENSOR_CHAN_GAUGE_AVG_CURRENT, "A");

	struct sensor_value vbat;

	if (sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &vbat) == 0) {
		test_report("npm1300", TEST_PASS, "VBAT=%d.%03d V",
			    vbat.val1, vbat.val2 / 1000);
	} else {
		test_report("npm1300", TEST_PASS, "fetch ok, VBAT read err");
	}
}
