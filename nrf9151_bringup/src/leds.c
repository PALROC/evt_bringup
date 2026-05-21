#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "leds.h"

LOG_MODULE_REGISTER(leds, LOG_LEVEL_INF);

static const struct gpio_dt_spec led_blue =
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_blue), gpios);
static const struct gpio_dt_spec led_green =
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_green), gpios);
static const struct gpio_dt_spec led_red =
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_red), gpios);

void boot_indicator(void)
{
	if (!gpio_is_ready_dt(&led_red)) {
		return;
	}
	gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	gpio_pin_set_dt(&led_red, 1);
	k_msleep(1000);
	gpio_pin_set_dt(&led_red, 0);
}

static void blink_one(const struct gpio_dt_spec *led, const char *name)
{
	if (!gpio_is_ready_dt(led)) {
		LOG_ERR("led %s not ready", name);
		return;
	}

	int ret = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);

	if (ret) {
		LOG_ERR("led %s configure failed: %d", name, ret);
		return;
	}

	for (int i = 0; i < 3; i++) {
		LOG_INF("  %s ON",  name); gpio_pin_set_dt(led, 1); k_msleep(1000);
		LOG_INF("  %s OFF", name); gpio_pin_set_dt(led, 0); k_msleep(1000);
	}
}

void leds_walk(void)
{
	LOG_INF("LED walk: blue -> green -> red (3x each)");
	blink_one(&led_blue,  "blue");
	blink_one(&led_green, "green");
	blink_one(&led_red,   "red");
}
