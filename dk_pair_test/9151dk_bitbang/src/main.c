/*
 * nRF9151 DK BIT-BANG read of the onboard GD25WB256 flash.
 *
 * Bypasses the Zephyr SPI driver and the SPIM3 peripheral entirely.
 * Drives SCK/MOSI/CS as plain GPIO outputs and samples MISO as plain
 * GPIO input. Just like the bit-bang section in flash_test_evt2.
 *
 * 9151 DK onboard GD25 pin map:
 *   SCK  = P0.13   (D13 on the Arduino-style header)
 *   MISO = P0.12   (D12)
 *   MOSI = P0.11   (D11)
 *   CS   = P0.20   (internal — onboard flash /CS net)
 *   JEDEC = c8 65 19  (GigaDevice GD25WB256, 256 Mbit)
 *
 * Use case: ground-truth sanity check. If THIS reads c8 65 19, then
 * the 9151 DK's GPIO pins genuinely reach the onboard flash chip
 * (separate from whether SPIM3 hardware works). Useful when debugging
 * weird symptoms on the bus.
 *
 * Build:
 *   west build -p -b nrf9151dk/nrf9151/ns -d build
 *   west flash -d build --snr <9151_DK_serial>
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define SCK_PIN   13
#define MISO_PIN  12
#define MOSI_PIN  11
#define CS_PIN    20

#define CMD_JEDEC 0x9F

/* Bit-bang one byte out on MOSI while sampling MISO. SPI mode 0,
 * MSB-first, ~125 kHz (k_busy_wait(2) per half-cycle). */
static uint8_t bb_xchg(const struct device *gpio0, uint8_t out)
{
	uint8_t in = 0;
	for (int bit = 7; bit >= 0; bit--) {
		gpio_pin_set(gpio0, MOSI_PIN, (out >> bit) & 1);
		k_busy_wait(2);
		gpio_pin_set(gpio0, SCK_PIN, 1);
		k_busy_wait(2);
		in |= (gpio_pin_get(gpio0, MISO_PIN) & 1) << bit;
		gpio_pin_set(gpio0, SCK_PIN, 0);
		k_busy_wait(2);
	}
	return in;
}

int main(void)
{
	printk("\n");
	printk(">>> BUILD-TAG: dk_pair 9151dk_bitbang 2026-05-26\n");
	printk(">>> 9151 DK bit-bang reading onboard GD25WB256\n");
	printk(">>> Pins (as plain GPIOs): SCK=P0.%d MOSI=P0.%d MISO=P0.%d CS=P0.%d\n",
	       SCK_PIN, MOSI_PIN, MISO_PIN, CS_PIN);
	printk(">>> Expected JEDEC: c8 65 19 (GD25WB256)\n");
	printk(">>> Polling every 5 s.\n");

	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (!device_is_ready(gpio0)) {
		printk(">>> ERR: gpio0 not ready\n");
		return -ENODEV;
	}

	/* Configure the four pins as plain GPIOs. Idle: CS high, SCK low,
	 * MOSI low, MISO input (with light pull-up to detect open lines). */
	gpio_pin_configure(gpio0, CS_PIN,   GPIO_OUTPUT_HIGH);
	gpio_pin_configure(gpio0, SCK_PIN,  GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MOSI_PIN, GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MISO_PIN, GPIO_INPUT | GPIO_PULL_UP);

	k_msleep(50);

	uint32_t iter = 0;
	while (1) {
		iter++;

		/* Assert CS. */
		gpio_pin_set(gpio0, CS_PIN, 0);
		k_busy_wait(5);

		/* Send 0x9F + 3 dummy bytes, sample 4 bytes back. */
		uint8_t rx[4];
		rx[0] = bb_xchg(gpio0, CMD_JEDEC);
		rx[1] = bb_xchg(gpio0, 0x00);
		rx[2] = bb_xchg(gpio0, 0x00);
		rx[3] = bb_xchg(gpio0, 0x00);

		gpio_pin_set(gpio0, CS_PIN, 1);
		k_busy_wait(5);

		bool ok = (rx[1] == 0xC8 && rx[2] == 0x65 && rx[3] == 0x19);
		printk(">>> iter %u: BITBANG JEDEC %02x %02x %02x   %s  (raw byte0=%02x)\n",
		       iter, rx[1], rx[2], rx[3],
		       ok ? "PASS (GD25WB256)" :
		       (rx[1] == 0 && rx[2] == 0 && rx[3] == 0)
			   ? "FAIL (no response — wire / chip / CS issue)" :
			   "(unexpected — possible SI / contention)",
		       rx[0]);

		k_msleep(5000);
	}

	return 0;
}
