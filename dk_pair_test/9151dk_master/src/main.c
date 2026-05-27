/*
 * nRF9151 DK master — reads onboard GD25WB256 flash via SPI3.
 *
 * Part of the DK-pair reproducer for the EVT2 SPIM3 silent-failure
 * bug. Pairs with `../l15dk_observer/` running on a wired nRF54L15
 * DK. See `../README.md` for the full test matrix and wiring.
 *
 * The 9151 DK's onboard flash is on:
 *   SPI3:  SCK=P0.13  MOSI=P0.11  MISO=P0.12
 *   CS:    P0.20      (internal, only on the 9151 DK board)
 *   JEDEC: c8 65 19   (GigaDevice GD25WB256, 256 Mbit)
 *
 * This app uses the SPI3 device defined in the board's common.dtsi
 * (which already has the chip wired) — we just call spi_transceive
 * and print the result.
 *
 * Build:
 *   west build -p -b nrf9151dk/nrf9151/ns -d build
 *   west flash -d build --snr <9151_DK_serial>
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

/* Flip this to choose which chip we're reading:
 *   USE_EXTERNAL_W25 = 1  ->  external Winbond/EON W25Q chip at CS = P0.10
 *                             (Arduino D10 on the 9151 DK; matches EVT2's
 *                              W25Q128JV chip family — same protocol).
 *   USE_EXTERNAL_W25 = 0  ->  onboard GD25WB256 at CS = P0.20
 *                             (used in tests A-D, already confirmed working).
 */
#define USE_EXTERNAL_W25  0

#if USE_EXTERNAL_W25
#define FLASH_CS_PIN      10
#define FLASH_NAME        "external W25Q (Winbond-family) at CS=P0.10"
#define EXPECTED_JEDEC0   0xEF   /* Winbond */
#define EXPECTED_JEDEC0B  0x1C   /* or EON (fake-Winbond Amazon variant) */
#define EXPECTED_JEDEC1   0x40   /* W25Q standard SPI family */
#define EXPECTED_JEDEC1B  0x30   /* EON family code */
#else
#define FLASH_CS_PIN      20
#define FLASH_NAME        "onboard GD25WB256 at CS=P0.20"
#define EXPECTED_JEDEC0   0xC8   /* GigaDevice */
#define EXPECTED_JEDEC0B  0xC8
#define EXPECTED_JEDEC1   0x65
#define EXPECTED_JEDEC1B  0x65
#endif

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

int main(void)
{
	printk("\n");
	printk(">>> BUILD-TAG: dk_pair 9151dk_master 2026-05-26 (USE_EXTERNAL_W25=%d)\n",
	       USE_EXTERNAL_W25);
	printk(">>> 9151 DK reading: %s\n", FLASH_NAME);
	printk(">>> pins: SCK=P0.13  MOSI=P0.11  MISO=P0.12  CS=P0.%d\n",
	       FLASH_CS_PIN);
	printk(">>> Polling JEDEC every 5 s.\n");

	if (!device_is_ready(spi)) {
		printk(">>> ERR: spi3 not ready\n");
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
		if (ret) {
			printk(">>> iter %u: spi err %d\n", iter, ret);
		} else {
			uint8_t mfg = rx[1], type = rx[2], cap = rx[3];
			bool ok = ((mfg == EXPECTED_JEDEC0  && type == EXPECTED_JEDEC1) ||
				   (mfg == EXPECTED_JEDEC0B && type == EXPECTED_JEDEC1B));
			printk(">>> iter %u: JEDEC %02x %02x %02x   %s\n",
			       iter, mfg, type, cap,
			       ok ? "PASS" :
			       (mfg == 0 && type == 0 && cap == 0)
				   ? "FAIL (no clocks)" :
				   "(unexpected)");
		}
		k_msleep(5000);
	}

	return 0;
}
