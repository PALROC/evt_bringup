#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "button_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(button, LOG_LEVEL_INF);

/* Two hall sensors, both on GPIO0 (names per the schematic):
 *   hall2 = P0.01 — characterised: active-LOW (open-drain), pin LOW = pressed
 *   hall1 = P0.00 — KNOWN BROKEN on the current board (assembly issue;
 *                   parked, fix on next board spin / rework). Probe still
 *                   reads it so we have a record, but transitions on hall1
 *                   are not expected to make sense.
 *
 * Each sensor mirrors to a different LED so you can wave a magnet over each
 * one independently and tell them apart visually. Red is reserved for the
 * boot indicator + future error/alarm signalling, so we use:
 *   hall2 → blue  LED
 *   hall1 → green LED  (will not toggle in practice — sensor is broken)
 *
 * The probe logs each transition with timestamp + sensor ID + new level.
 */
#define HALL2_PIN  1
/* FIXME(hardware): hall1 (P0.00) is broken on this board — likely an
 * assembly defect (same class as the LED MOSFET issue / SPI3 traces).
 * The pin is read so we can keep a record of its state in the log, but
 * its transitions are not expected to make sense. To be fixed on the
 * next board spin / rework. Don't rely on hall1 for any production logic
 * until then.
 */
#define HALL1_PIN  0

#define LED_BLUE_NODE  DT_ALIAS(led_blue)
#define LED_GREEN_NODE DT_ALIAS(led_green)

static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(LED_BLUE_NODE,  gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);

void button_probe(int duration_seconds)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready");
		return;
	}

	/* Internal pull-up is safe with both push-pull and open-drain
	 * sensors: a push-pull output overrides our weak pull-up easily,
	 * and an open-drain sensor's HIGH state needs the pull-up to be
	 * defined at all.
	 */
	int err0 = gpio_pin_configure(gpio0, HALL2_PIN, GPIO_INPUT | GPIO_PULL_UP);
	int err1 = gpio_pin_configure(gpio0, HALL1_PIN, GPIO_INPUT | GPIO_PULL_UP);

	if (err0 || err1) {
		LOG_ERR("gpio_pin_configure: hall2=%d hall1=%d", err0, err1);
		return;
	}

	if (device_is_ready(led_blue.port)) {
		gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_INACTIVE);
	}
	if (device_is_ready(led_green.port)) {
		gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	}

	int last0 = gpio_pin_get(gpio0, HALL2_PIN);
	int last1 = gpio_pin_get(gpio0, HALL1_PIN);
	int trans0 = 0;
	int trans1 = 0;
	int64_t end_at = k_uptime_get() + duration_seconds * 1000LL;
	int64_t next_heartbeat = k_uptime_get() + 1000LL;

	LOG_INF("--- hall probe: hall2=P0.%d (blue LED), hall1=P0.%d (green LED), %d s ---",
		HALL2_PIN, HALL1_PIN, duration_seconds);
	LOG_WRN("  hall1 is KNOWN BROKEN on this board — transitions on it should be ignored");
	LOG_INF("  resting levels: hall2=%d  hall1=%d", last0, last1);

	while (k_uptime_get() < end_at) {
		int level0 = gpio_pin_get(gpio0, HALL2_PIN);
		int level1 = gpio_pin_get(gpio0, HALL1_PIN);

		if (level0 != last0) {
			trans0++;
			LOG_INF("  t=%5lldms  hall2 (P0.%d) %d → %d  (transitions: %d)",
				k_uptime_get(), HALL2_PIN, last0, level0, trans0);
			last0 = level0;
		}
		if (level1 != last1) {
			trans1++;
			LOG_INF("  t=%5lldms  hall1 (P0.%d) %d → %d  (transitions: %d)",
				k_uptime_get(), HALL1_PIN, last1, level1, trans1);
			last1 = level1;
		}

		/* LEDs mirror raw pin level: 1 = LED on, 0 = LED off.
		 * With the active-LOW hall2 sensor at rest, blue LED is on;
		 * magnet near drops the pin to 0 and the LED turns off. The
		 * LED tracks the line state, not "magnet near".
		 */
		if (device_is_ready(led_blue.port)) {
			gpio_pin_set_dt(&led_blue,  level0);
		}
		if (device_is_ready(led_green.port)) {
			gpio_pin_set_dt(&led_green, level1);
		}

		if (k_uptime_get() >= next_heartbeat) {
			LOG_INF("  hb: hall2=%d (trans %d)  hall1=%d (trans %d)",
				level0, trans0, level1, trans1);
			next_heartbeat += 1000LL;
		}

		k_msleep(2);
	}

	if (device_is_ready(led_blue.port)) {
		gpio_pin_set_dt(&led_blue, 0);
	}
	if (device_is_ready(led_green.port)) {
		gpio_pin_set_dt(&led_green, 0);
	}

	LOG_INF("--- hall probe complete: hall2=%d transitions, hall1=%d transitions in %d s ---",
		trans0, trans1, duration_seconds);
	if (trans0 == 0 && trans1 == 0) {
		LOG_WRN("  no transitions on either pin — magnet weak / wrong pins / sensors not connected?");
	}

	/* hall2 is the working sensor — pass criterion is "any transition seen
	 * during the probe" (i.e. user actually waved a magnet). hall1 is
	 * known-broken on this board and reports as INFO regardless.
	 */
	test_report("hall2", trans0 > 0 ? TEST_PASS : TEST_FAIL,
		    "%d transitions in %ds, rest=%d",
		    trans0, duration_seconds, last0);
	test_report("hall1", TEST_INFO,
		    "%d transitions (sensor known broken)", trans1);
}
