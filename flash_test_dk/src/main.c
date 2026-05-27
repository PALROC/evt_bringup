/*
 * Dual SPI flash JEDEC ID probe for the nRF9151 DK.
 *
 * Probes TWO chips every 5 s on the DK's SPI3 bus:
 *
 *   1) ONBOARD: GD25WB256EYIG (256 Mbit GigaDevice), CS = P0.20.
 *      Always-present known-good reference. Should always read EF... no
 *      sorry, C8 65 19. If THIS reads anything other than C8 65 19, the
 *      DK / firmware / SPI driver itself has a problem and any external
 *      result is meaningless.
 *
 *   2) WIRED:   whatever you've wired to the Arduino header SPI pins,
 *      CS = P0.10 (D10). Expected JEDEC for Winbond W25Q family:
 *         EF 40 17 = W25Q64    (the fresh chips from Amazon)
 *         EF 40 18 = W25Q128JV (the EVT2 chip)
 *
 * Same SPI3 bus shared between both — only CS differs, so no contention.
 *
 * ===========================================================
 * Wiring for the WIRED chip (DK Arduino header -> W25Q SOIC-8)
 * ===========================================================
 *
 *     W25Q pin        |  function     |  Wire to DK header
 *     ----------------+---------------+------------------------
 *     1   /CS         |  Chip Select  |  D10  (P0.10)
 *     2   DO          |  MISO         |  D12  (P0.12)
 *     3   /WP         |  Write Prot.  |  3V3  (tie high — 10k or bridge)
 *     4   GND         |  Ground       |  GND
 *     5   DI          |  MOSI         |  D11  (P0.11)
 *     6   CLK         |  Clock        |  D13  (P0.13 / SCK)
 *     7   /HOLD       |  Hold         |  3V3  (tie high — same as /WP)
 *     8   VCC         |  3.3V power   |  3V3
 *
 *   IMPORTANT: pin 1 (/CS) goes to D10, NOT VCC. The DK drives it low
 *   to select the chip; a hard tie to VCC keeps it permanently
 *   deselected. /WP and /HOLD on the other hand can be bridged
 *   straight to VCC — in plain SPI they just need to sit high.
 * ===========================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_test, LOG_LEVEL_INF);

#define SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));

/* Wired flash on D10 / P0.10 — whatever you've connected externally. */
static struct spi_config cfg_wired = {
	.frequency = 1000000,
	.operation = SPI_OP,
	.cs = {
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = 10,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};

/* Onboard flash GD25WB256EYIG on the 9151 DK — CS = P0.20 (from the
 * board DT). Same SPI3, different CS, so no bus contention with the
 * wired chip — only one chip is selected per transfer. */
static struct spi_config cfg_onboard = {
	.frequency = 1000000,
	.operation = SPI_OP,
	.cs = {
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = 20,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};

/* JEDEC manufacturer IDs we recognise. Cheap Amazon "W25Q64" chips are
 * often actually EON EN25Q64A — pin/protocol-compatible clones with a
 * different mfg ID. Same SPI commands work, so they're a valid drop-in. */
#define JEDEC_MFG_WINBOND    0xEF
#define JEDEC_MFG_GIGADEVICE 0xC8
#define JEDEC_MFG_EON        0x1C

#define CMD_RDP   0xAB /* Release from Deep Power-Down */
#define CMD_JEDEC 0x9F

/* Decode (mfg, type, density) into a human-readable part name. */
static const char *decode_part(uint8_t mfg, uint8_t type, uint8_t cap)
{
	if (mfg == JEDEC_MFG_WINBOND && type == 0x40) {
		switch (cap) {
		case 0x14: return "Winbond W25Q80   (8 Mbit)";
		case 0x15: return "Winbond W25Q16   (16 Mbit)";
		case 0x16: return "Winbond W25Q32   (32 Mbit)";
		case 0x17: return "Winbond W25Q64   (64 Mbit)";
		case 0x18: return "Winbond W25Q128  (128 Mbit)";
		default:   return "Winbond W25Q (unknown density)";
		}
	}
	if (mfg == JEDEC_MFG_GIGADEVICE && type == 0x65) {
		switch (cap) {
		case 0x18: return "GigaDevice GD25WB128 (128 Mbit)";
		case 0x19: return "GigaDevice GD25WB256 (256 Mbit) — DK onboard";
		default:   return "GigaDevice GD25 (unknown density)";
		}
	}
	if (mfg == JEDEC_MFG_EON && type == 0x30) {
		switch (cap) {
		case 0x15: return "EON EN25Q16  (16 Mbit, W25Q-compatible)";
		case 0x16: return "EON EN25Q32  (32 Mbit, W25Q-compatible)";
		case 0x17: return "EON EN25Q64  (64 Mbit, W25Q-compatible)";
		case 0x18: return "EON EN25Q128 (128 Mbit, W25Q-compatible)";
		default:   return "EON EN25Q (unknown density, W25Q-compatible)";
		}
	}
	if (mfg == 0x00 || mfg == 0xFF) {
		return "(no chip responding)";
	}
	return "(unrecognised mfg/type)";
}

/* Send RDP (0xAB) to wake the chip from deep power-down; harmless
 * no-op if it was already awake. */
static void wake_from_dpd(struct spi_config *cfg)
{
	uint8_t cmd = CMD_RDP;
	const struct spi_buf tx_buf = { .buf = &cmd, .len = 1 };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };

	(void)spi_write(spi, cfg, &txs);
	k_usleep(10);
}

/* Read JEDEC ID from the chip selected by `cfg`, log it with a friendly
 * label. Returns true if a valid Winbond or GigaDevice ID was read. */
static bool probe_jedec(struct spi_config *cfg, const char *label)
{
	wake_from_dpd(cfg);

	uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
	uint8_t rx[4] = { 0 };
	const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

	int ret = spi_transceive(spi, cfg, &txs, &rxs);
	if (ret) {
		LOG_ERR("%-8s spi err %d", label, ret);
		return false;
	}

	uint8_t mfg = rx[1], type = rx[2], cap = rx[3];
	const char *part = decode_part(mfg, type, cap);
	bool ok = (mfg == JEDEC_MFG_WINBOND && type == 0x40) ||
		  (mfg == JEDEC_MFG_GIGADEVICE && type == 0x65) ||
		  (mfg == JEDEC_MFG_EON && type == 0x30);

	LOG_INF("%-8s JEDEC %02x %02x %02x   %s  %s",
		label, mfg, type, cap, ok ? "PASS" : "----",
		part);
	return ok;
}

int main(void)
{
	LOG_INF("=================================================");
	LOG_INF("Dual flash JEDEC probe on 9151DK SPI3");
	LOG_INF("  ONBOARD: GD25WB256EYIG @ CS=P0.20   (known-good)");
	LOG_INF("  WIRED:   whatever you've wired @ CS=P0.10 (D10)");
	LOG_INF("  SCK=P0.13  MOSI=P0.11  MISO=P0.12  (Arduino header)");
	LOG_INF("Polling both every 5 seconds.");
	LOG_INF("=================================================");

	if (!device_is_ready(spi)) {
		LOG_ERR("spi3 not ready — overlay probably not applied");
		return -ENODEV;
	}

	k_msleep(50);

	int iter = 0;
	while (1) {
		iter++;
		LOG_INF("--- iter %d ---", iter);
		(void)probe_jedec(&cfg_onboard, "ONBOARD");
		(void)probe_jedec(&cfg_wired,   "WIRED  ");
		k_msleep(5000);
	}

	return 0;
}
