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

/* Ground-truth bit-bang JEDEC read on the same four pins as plain
 * GPIOs. Run ONCE at boot, BEFORE the SPI driver init claims the
 * pins — this avoids the pin-hijack confound. Reports whether the
 * chip is alive and the traces are intact, independent of SPIM3. */
static void bitbang_jedec_probe(void)
{
	printk(">>> --- BIT-BANG JEDEC probe (pre-SPI init) ---\n");

	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (!device_is_ready(gpio0)) {
		printk(">>> ERR: gpio0 not ready\n");
		return;
	}

	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(gpio0, SCK_PIN,      GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MISO_PIN,     GPIO_INPUT);
	k_msleep(5);

	gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
	k_busy_wait(5);
	uint8_t rx[4];
	rx[0] = bb_xchg(gpio0, CMD_JEDEC);
	rx[1] = bb_xchg(gpio0, 0x00);
	rx[2] = bb_xchg(gpio0, 0x00);
	rx[3] = bb_xchg(gpio0, 0x00);
	gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
	k_busy_wait(5);

	bool ok = (rx[1] == 0xEF && rx[2] == 0x40);
	printk(">>> [t=%lld ms] BITBANG JEDEC: %02x %02x %02x   %s (raw byte0=%02x)\n",
	       k_uptime_get(), rx[1], rx[2], rx[3],
	       ok ? "PASS — chip + traces alive" :
	       (rx[1] == 0 && rx[2] == 0 && rx[3] == 0)
		   ? "FAIL — even bit-bang gets nothing" :
		   "(unexpected — SI / wiring marginal)",
	       rx[0]);

	/* Release the pins back to high-Z input so the SPI driver's
	 * pinctrl can take them over cleanly when we call
	 * spi_transceive() next. */
	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, SCK_PIN,      GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, MISO_PIN,     GPIO_DISCONNECTED);
	k_msleep(20);

	printk(">>> --- end bit-bang probe; SPI3 driver now takes pins ---\n");
}

int main(void)
{
	printk("\n");
	printk(">>> BUILD-TAG: flash_test_evt2_minimal 2026-05-26\n");
	printk(">>> EVT2 minimal flash test — only SPI3 active\n");
	printk(">>> pins: SCK=P0.13  MOSI=P0.15  MISO=P0.14  CS=P0.%d\n",
	       FLASH_CS_PIN);

	/* Bit-bang probe re-enabled — run it once at boot to prove the
	 * chip + traces are alive, then wait an extra second so any pin /
	 * peripheral state has time to settle before the SPI driver
	 * claims the pins on its first transfer. */
	bitbang_jedec_probe();

	printk(">>> [t=%lld ms] Settling 1000 ms before handing pins to SPI3 driver...\n",
	       k_uptime_get());
	k_msleep(1000);

	printk(">>> [t=%lld ms] Polling SPI3 JEDEC. iter spacing: 10ms/100ms/1s/5s steady.\n",
	       k_uptime_get());

	if (!device_is_ready(spi)) {
		printk(">>> ERR: spi3 not ready\n");
		return -ENODEV;
	}
	/* Generous settle: wait 500 ms AFTER confirming the driver is
	 * ready and BEFORE the first transfer, to let any pin / pinctrl /
	 * peripheral state finish settling. */
	k_msleep(500);

	uint32_t iter = 0;
	while (1) {
		iter++;

		uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };
		const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
		const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
		const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
		const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

		/* Capture timestamps so we can see exactly when the wedge
		 * happens — does iter 2 fail immediately (peripheral wedged
		 * by iter 1) or only after the 5 s sleep (something happens
		 * during idle)? */
		int64_t t_before = k_uptime_get();
		int ret = spi_transceive(spi, &cfg, &txs, &rxs);
		int64_t t_after  = k_uptime_get();

		if (ret) {
			printk(">>> [t=%lld ms] iter %u: spi err %d (took %lld ms)\n",
			       t_before, iter, ret, t_after - t_before);
		} else {
			uint8_t mfg = rx[1], type = rx[2], cap = rx[3];
			bool ok = (mfg == 0xEF && type == 0x40);
			printk(">>> [t=%lld ms] iter %u: JEDEC %02x %02x %02x   %s (xfer=%lld ms)\n",
			       t_before, iter, mfg, type, cap,
			       ok ? "PASS" :
			       (mfg == 0 && type == 0 && cap == 0)
				   ? "FAIL (no clocks)" :
				   "(unexpected)",
			       t_after - t_before);
		}

		/* Vary the sleep: tight on iter 1->2 (10 ms) to test "wedge
		 * is immediate", then more spaced to see the steady-state
		 * pattern. */
		if (iter == 1) {
			k_msleep(10);    /* iter 2 hits in 10 ms */
		} else if (iter == 2) {
			k_msleep(100);   /* iter 3 in 100 ms */
		} else if (iter == 3) {
			k_msleep(1000);  /* iter 4 in 1 s */
		} else {
			k_msleep(5000);  /* steady state from iter 5 onwards */
		}
	}

	return 0;
}
