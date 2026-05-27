/*
 * SPI flash JEDEC ID probe for the EVT1 board.
 *
 * Goal: definitively answer "is the EVT1 board's SPI3 healthy, or is
 * there damage beyond just the dead Winbond chip?" by reading JEDEC
 * from whatever chip is currently soldered/wired to EVT1's flash CS.
 *
 * EVT1 SPI3 pin map (from palroc_nova/boards/.../nrf9151_nova_evt1-pinctrl.dtsi):
 *     SCK  = P0.13
 *     MOSI = P0.15
 *     MISO = P0.14
 *     CS   = P0.12   (the original W25Q128JV footprint)
 *
 * The nRF54L15 shares the same SPI3 nets on this board. Its nRESET pin
 * (P0.19) has a 10 kOhm pull-up so it boots by default. We deliberately
 * leave it alone here — the L15 boots freely. SPI3 is therefore shared
 * exactly as it would be in a real bring-up run, which is what we want
 * to characterise (does the EVT1 SPI3 still work when the L15 is up?).
 *
 * ===========================================================
 * Use cases this app covers:
 *
 *  1) Read the EXISTING (likely damaged) Winbond chip in place at CS=P0.12.
 *     Expected: 00 00 00 or shifted/corrupt bytes (e.g. EE 00 40 we saw on
 *     the DK). That just confirms the chip is dead — already known.
 *
 *  2) Wire a FRESH chip (EON EN25Q64 from Amazon, or a genuine Winbond)
 *     to EVT1's SPI3 nets after removing the dead chip. If it reads a
 *     valid JEDEC, the EVT1 board is electrically fine -> chip damage
 *     was the only issue; same fix will work on EVT2.
 *
 *  3) For (2), recognised JEDEC IDs and what they mean:
 *       EF 40 17 -> Winbond W25Q64  (genuine fresh part)
 *       EF 40 18 -> Winbond W25Q128 (the original part type)
 *       1C 30 17 -> EON EN25Q64 (the "fake W25Q64" Amazon chips we tested)
 *       C8 65 19 -> GigaDevice GD25WB256 (e.g. transplanted from a DK)
 *
 * Build:
 *   cd evt_bringup/flash_test_evt1
 *   west build -p -b nrf9151_nova_evt1/nrf9151/ns -d build \
 *       -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
 *   west flash -d build
 * ===========================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_test, LOG_LEVEL_INF);

#define FLASH_CS_PIN    12

#define SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));

static struct spi_config cfg_flash = {
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

#define JEDEC_MFG_WINBOND    0xEF
#define JEDEC_MFG_GIGADEVICE 0xC8
#define JEDEC_MFG_EON        0x1C

#define CMD_RDP   0xAB
#define CMD_JEDEC 0x9F

static const char *decode_part(uint8_t mfg, uint8_t type, uint8_t cap)
{
	if (mfg == JEDEC_MFG_WINBOND && type == 0x40) {
		switch (cap) {
		case 0x14: return "Winbond W25Q80   (8 Mbit)";
		case 0x15: return "Winbond W25Q16   (16 Mbit)";
		case 0x16: return "Winbond W25Q32   (32 Mbit)";
		case 0x17: return "Winbond W25Q64   (64 Mbit)";
		case 0x18: return "Winbond W25Q128  (128 Mbit) — original EVT1 part";
		default:   return "Winbond W25Q (unknown density)";
		}
	}
	if (mfg == JEDEC_MFG_GIGADEVICE && type == 0x65) {
		switch (cap) {
		case 0x19: return "GigaDevice GD25WB256 (256 Mbit)";
		default:   return "GigaDevice GD25 (unknown density)";
		}
	}
	if (mfg == JEDEC_MFG_EON && type == 0x30) {
		switch (cap) {
		case 0x17: return "EON EN25Q64  (64 Mbit, fake-Winbond Amazon chip)";
		case 0x18: return "EON EN25Q128 (128 Mbit, W25Q-compatible)";
		default:   return "EON EN25Q (unknown density)";
		}
	}
	if (mfg == 0x00 || mfg == 0xFF) {
		return "(no chip responding — wiring fault or chip absent)";
	}
	return "(unrecognised mfg/type — corrupt response, likely damaged chip)";
}

static void wake_from_dpd(void)
{
	uint8_t cmd = CMD_RDP;
	const struct spi_buf tx_buf = { .buf = &cmd, .len = 1 };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };

	(void)spi_write(spi, &cfg_flash, &txs);
	k_usleep(10);
}

int main(void)
{
	LOG_INF("=================================================");
	LOG_INF("EVT1 SPI flash JEDEC probe");
	LOG_INF("  SPI3: SCK=P0.13  MOSI=P0.15  MISO=P0.14  CS=P0.%d",
		FLASH_CS_PIN);
	LOG_INF("  L15 booting freely (nRESET not asserted by this app).");
	LOG_INF("Polls every 5 s.");
	LOG_INF("=================================================");

	if (!device_is_ready(spi)) {
		LOG_ERR("spi3 not ready");
		return -ENODEV;
	}

	k_msleep(50);
	wake_from_dpd();

	uint32_t iter = 0;
	while (1) {
		iter++;

		uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };
		const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
		const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
		const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
		const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

		int ret = spi_transceive(spi, &cfg_flash, &txs, &rxs);
		if (ret) {
			LOG_ERR("iter %u: spi err %d", iter, ret);
			k_msleep(5000);
			continue;
		}

		uint8_t mfg = rx[1], type = rx[2], cap = rx[3];
		bool ok = (mfg == JEDEC_MFG_WINBOND    && type == 0x40) ||
			  (mfg == JEDEC_MFG_GIGADEVICE && type == 0x65) ||
			  (mfg == JEDEC_MFG_EON        && type == 0x30);

		LOG_INF("iter %u: JEDEC %02x %02x %02x   %s  %s",
			iter, mfg, type, cap, ok ? "PASS" : "----",
			decode_part(mfg, type, cap));

		k_msleep(5000);
	}

	return 0;
}
