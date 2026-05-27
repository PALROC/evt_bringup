/*
 * nRF54L15 DK observer — the "second MCU on the shared SPI bus" half
 * of the DK-pair reproducer. Pairs with ../9151dk_master/.
 *
 * Two modes, switch via the #define below:
 *
 *   MODE_IDLE = 1 (default):
 *     Boot Zephyr + TF-M fully, then sleep forever. SPI peripheral
 *     is NOT initialized; all GPIOs default to high-Z input. This
 *     is the "L15 is alive but doing nothing" test case (test C in
 *     ../README.md) — matches the EVT2 case where the L15 is up but
 *     not driving the bus.
 *
 *   MODE_IDLE = 0:
 *     Initialize spi00 on the same physical pins as the EVT2 board
 *     (P2.1 SCK / P2.2 MOSI / P2.4 MISO) and poll JEDEC every
 *     100 ms. Since no CS line reaches the 9151 DK's onboard flash,
 *     these transfers never actually select the flash — they just
 *     drive SCK/MOSI bursts onto the shared bus to fight the 9151's
 *     transactions. This is the worst-case contention test (test D).
 *
 * Build:
 *   west build -p -b nrf54l15dk/nrf54l15/cpuapp/ns -d build
 *   west flash -d build --snr <L15_DK_serial>
 *
 * SPIM00 quirks on nRF54L (we hit these in flash_test_l15_evt2):
 *   - SPIM00 minimum frequency 1.016 MHz (128 MHz core / max
 *     prescaler 126). Anything below that returns NRFX_ERROR_INVALID_PARAM.
 *   - TF-M owns the peripheral on /ns; the prj.conf already has the
 *     CONFIG_NRF_SPIM00_SECURE=n release.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>

#define MODE_IDLE 0   /* 1 = passive observer; 0 = active SCK/MOSI burst */

#define CMD_JEDEC 0x9F

#if !MODE_IDLE
#define SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi00));

static struct spi_config cfg = {
	/* SPIM00 minimum frequency is 1.016 MHz (128 MHz / max prescaler
	 * 126). Setting EXACTLY at that boundary made spi_transceive hang
	 * (driver round-off pushes prescaler to 127 = invalid). Use 2 MHz
	 * — prescaler 64, exact match, well within range, and still slow
	 * enough to tolerate long flying-wire bridges between DKs. */
	.frequency = 2000000,
	.operation = SPI_OP,
	.cs = {
		.gpio = {
			/* P1.10 — wired by flying jumper to the external W25's
			 * /CS in parallel with the 9151 DK's P0.10. Both MCUs
			 * select the same chip; recreates EVT2's shared-CS
			 * topology. */
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
			.pin = 10,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};
#endif

int main(void)
{
	printk("\n");
	printk(">>> BUILD-TAG: dk_pair l15dk_observer 2026-05-26\n");

#if MODE_IDLE
	printk(">>> MODE: IDLE — L15 is alive but does nothing\n");
	printk(">>> All GPIOs default to high-Z input.\n");
	printk(">>> Watch RTT on the 9151 DK to see if its SPI3 still works.\n");

	while (1) {
		k_msleep(10000);
		printk(">>> (still idle, t=%lld s)\n",
		       k_uptime_get() / 1000);
	}
#else
	printk(">>> MODE: POLL — L15 reads the GD25 on the 9151 DK via shared bus\n");
	printk(">>> spi00 pins: SCK=P2.1 MOSI=P2.2 MISO=P2.4 (jumpered to 9151 DK)\n");
	printk(">>> CS=P1.10 (jumper to 9151 DK P0.20 = GD25 /CS)\n");
	printk(">>> Expected JEDEC: c8 65 19 (GD25WB256)\n");

	if (!device_is_ready(spi)) {
		printk(">>> ERR: spi00 not ready\n");
		return -ENODEV;
	}
	k_msleep(50);

	uint32_t iter = 0;
	while (1) {
		iter++;

		uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };
		const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
		const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
		const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
		const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

		int ret = spi_transceive(spi, &cfg, &txs, &rxs);
		/* Print every iter while we debug the long-wire situation —
		 * we need immediate feedback, not buffered every 5 s. */
		if (iter % 5 == 0) {
			printk(">>> L15 iter %u  (ret=%d, rx=%02x %02x %02x)\n",
			       iter, ret, rx[1], rx[2], rx[3]);
		}
		k_msleep(500);   /* slower poll: easier to read RTT */
	}
#endif

	return 0;
}
