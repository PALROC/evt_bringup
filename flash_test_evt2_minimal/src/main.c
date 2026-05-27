/*
 * EVT2 9151 minimal flash test — ONLY does SPI3 JEDEC reads.
 *
 * Companion to flash_test_evt2 but stripped of everything that
 * touches GPIO before SPI init. Specifically NO:
 *   - GPIO pin-health probe (was reconfiguring P0.12-15 as inputs
 *     before SPI3 was used; suspected pin-hijack confound)
 *   - Bit-bang preamble (same hijack risk)
 *   - SPIM3 register probe (irrelevant for the production fix)
 *   - Anything that touches any peripheral other than SPI3
 *
 * The overlay disables every other peripheral on the board (uart0,
 * sw-lpuart, i2c2/PMIC, spi1/nRF7000, all PWMs). prj.conf turns off
 * every subsystem we don't need.
 *
 * If THIS app reads `EF 40 18 PASS` on EVT2: the EVT2 hardware is
 * fine; our full bring-up config was contributing to the wedge.
 * If THIS app reads `00 00 00`: EVT2 hardware genuinely has the
 * same failure as the DK pair reproducer, even without any other
 * peripheral activity. That'd be the cleanest possible
 * "single-master, no contention, but SPIM3 still fails on EVT2"
 * data point.
 *
 * Build:
 *   cd evt_bringup/flash_test_evt2_minimal
 *   west build -p -b nrf9151_nova_evt2/nrf9151/ns -d build \
 *       -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
 *   west flash -d build
 *
 * For meaningful results, the L15 must NOT be running anything that
 * drives SPI3 — erase it or flash it with a silent firmware before
 * running this test.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_test, LOG_LEVEL_INF);

#define FLASH_CS_PIN  12
#define SCK_PIN       13
#define MISO_PIN      14
#define MOSI_PIN      15

#define SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));

static struct spi_config cfg = {
	.frequency = 1000000,
	.operation = SPI_OP,
	.cs = {
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = FLASH_CS_PIN,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};

#define CMD_JEDEC 0x9F

/* Parametrised bit-bang. SPI mode 0, MSB-first.
 *
 * Approximate bit-rate per delay value:
 *   delay_us = 4  ->  ~80 kHz   (period ~12 µs, very safe)
 *   delay_us = 2  -> ~125 kHz   (period ~8 µs — our original)
 *   delay_us = 1  -> ~250 kHz   (period ~4 µs)
 *   delay_us = 0  -> ~500 kHz-1 MHz (raw gpio_pin_set rate, no explicit
 *                                    delay; the call itself takes ~500ns)
 *
 * Real bit-rate depends on Zephyr's gpio_pin_set call overhead too.
 * For the very fastest (0), we skip k_busy_wait entirely and let the
 * three pin_set calls themselves time the cycle. */
static uint8_t bb_xchg(const struct device *gpio0, uint8_t out, uint32_t delay_us)
{
	uint8_t in = 0;
	for (int bit = 7; bit >= 0; bit--) {
		gpio_pin_set(gpio0, MOSI_PIN, (out >> bit) & 1);
		if (delay_us) {
			k_busy_wait(delay_us);
		}
		gpio_pin_set(gpio0, SCK_PIN, 1);
		if (delay_us) {
			k_busy_wait(delay_us);
		}
		in |= (gpio_pin_get(gpio0, MISO_PIN) & 1) << bit;
		gpio_pin_set(gpio0, SCK_PIN, 0);
		if (delay_us) {
			k_busy_wait(delay_us);
		}
	}
	return in;
}

/* Run one JEDEC read at a given delay and log the result. Cycle:
 * CS low -> 4 byte exchanges -> CS high. No prints inside the bit
 * loop itself (would mess with timing); only one LOG_INF at the end. */
static void bitbang_one_speed(const struct device *gpio0, uint32_t delay_us,
			      const char *label)
{
	int64_t t_before = k_uptime_get();

	gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
	k_busy_wait(5);

	uint8_t rx[4];
	rx[0] = bb_xchg(gpio0, CMD_JEDEC, delay_us);
	rx[1] = bb_xchg(gpio0, 0x00,      delay_us);
	rx[2] = bb_xchg(gpio0, 0x00,      delay_us);
	rx[3] = bb_xchg(gpio0, 0x00,      delay_us);

	gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
	k_busy_wait(5);

	int64_t t_after = k_uptime_get();

	bool ok = (rx[1] == 0xEF && rx[2] == 0x40);
	const char *verdict =
		ok ? "PASS" :
		(rx[1] == 0 && rx[2] == 0 && rx[3] == 0)
			? "FAIL (no response)" :
			"(corrupt)";

	LOG_INF("BB[%s d=%uus] JEDEC %02x %02x %02x  %s (byte0=%02x  took %lldms)",
		label, delay_us, rx[1], rx[2], rx[3], verdict, rx[0],
		t_after - t_before);
}

/* Sample MISO + SCK with internal pull-up enabled to make sure the bus
 * is truly idle (no chip / wire still actively driving). With CS high,
 * the chip's DO should be in high-Z, and our pull-up should win on
 * both lines. Reads should be 1 / 1.
 *
 * If MISO reads 0 with a pull-up, something is actively pulling MISO
 * low — could mean the chip is still in a transfer state, or the L15
 * (if it's somehow present) is driving the line. */
static void bus_idle_check(const struct device *gpio0, const char *when)
{
	/* Force pins to input + pull-up for the check. */
	gpio_pin_configure(gpio0, MISO_PIN, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure(gpio0, SCK_PIN,  GPIO_INPUT | GPIO_PULL_UP);
	k_msleep(2);

	int miso = gpio_pin_get(gpio0, MISO_PIN);
	int sck  = gpio_pin_get(gpio0, SCK_PIN);

	LOG_INF("BUS CHECK %s: MISO=%d SCK=%d (both should be 1 with pull-up = idle)",
		when, miso, sck);

	/* Restore SCK as output low + MISO as input (no pull) ready for
	 * the next bit-bang. */
	gpio_pin_configure(gpio0, SCK_PIN,  GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MISO_PIN, GPIO_INPUT);
}

/* Bit-bang JEDEC sweep — FAST first, then progressively slower.
 *
 * Between every test we:
 *   1. wait 2 seconds (so any chip-side state machine returns to idle)
 *   2. sample MISO + SCK with pull-up to confirm the bus is at rest
 *   3. then run the next bit-bang
 *
 * The structure exposes anything weird: if test N changes the bus's
 * idle state visible in the post-test check, we'll see it. */
static void bitbang_jedec_sweep(void)
{
	LOG_INF("--- BIT-BANG JEDEC sweep (fast first, with bus checks) ---");

	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready");
		return;
	}

	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(gpio0, SCK_PIN,      GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MISO_PIN,     GPIO_INPUT);
	k_msleep(50);

	/* Wake the chip from any possible deep-power-down state at very
	 * slow speed first. Harmless if it's already awake. */
	gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
	k_busy_wait(5);
	(void)bb_xchg(gpio0, 0xAB, 4);
	gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
	k_msleep(10);

	bus_idle_check(gpio0, "at-start (pre-fast)");
	k_msleep(2000);

	bitbang_one_speed(gpio0, 0, "fast");      /* no explicit delay */
	k_msleep(2000);
	bus_idle_check(gpio0, "after-fast");

	k_msleep(2000);
	bitbang_one_speed(gpio0, 1, "medium");    /* ~250 kHz target */
	k_msleep(2000);
	bus_idle_check(gpio0, "after-medium");

	k_msleep(2000);
	bitbang_one_speed(gpio0, 2, "slow");      /* ~125 kHz target */
	k_msleep(2000);
	bus_idle_check(gpio0, "after-slow");

	k_msleep(2000);
	bitbang_one_speed(gpio0, 4, "very-slow"); /* ~80 kHz target */
	k_msleep(2000);
	bus_idle_check(gpio0, "after-very-slow");

	/* Release the pins back to high-Z input. */
	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, SCK_PIN,      GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, MISO_PIN,     GPIO_DISCONNECTED);
	k_msleep(50);

	LOG_INF("--- end bit-bang sweep; SPI3 driver now takes pins ---");
}

int main(void)
{
	LOG_INF("==========================================");
	LOG_INF("BUILD-TAG: flash_test_evt2_minimal SWEEP-v2");
	LOG_INF("EVT2 minimal flash test — only SPI3 active");
	LOG_INF("pins: SCK=P0.13  MOSI=P0.15  MISO=P0.14  CS=P0.%d",
		FLASH_CS_PIN);
	LOG_INF("==========================================");

	/* Bit-bang sweep at multiple speeds with bus-idle checks between
	 * each, fast first. ~25 seconds total. */
	bitbang_jedec_sweep();

	LOG_INF("Settling 2 seconds before handing pins to SPI3 driver...");
	k_msleep(2000);

	if (!device_is_ready(spi)) {
		LOG_ERR("spi3 not ready");
		return -ENODEV;
	}
	k_msleep(500);

	LOG_INF("Polling SPI3 JEDEC every 5 s. Expected: ef 40 18 PASS.");

	uint32_t iter = 0;
	while (1) {
		iter++;

		uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };
		const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
		const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
		const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
		const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

		int64_t t_before = k_uptime_get();
		int ret = spi_transceive(spi, &cfg, &txs, &rxs);
		int64_t t_after  = k_uptime_get();

		if (ret) {
			LOG_ERR("iter %u: spi err %d (took %lld ms)",
				iter, ret, t_after - t_before);
		} else {
			uint8_t mfg = rx[1], type = rx[2], cap = rx[3];
			bool ok = (mfg == 0xEF && type == 0x40);
			LOG_INF("iter %u: JEDEC %02x %02x %02x  %s  (xfer=%lld ms)",
				iter, mfg, type, cap,
				ok ? "PASS" :
				(mfg == 0 && type == 0 && cap == 0)
				    ? "FAIL" :
				    "(?)",
				t_after - t_before);
		}

		k_msleep(5000);
	}

	return 0;
}
