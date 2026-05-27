#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_spim.h>

#include "spi_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(spi_probe, LOG_LEVEL_INF);

/* === Flash JEDEC path: RAW SPIM3 HAL =====================================
 *
 * 2026-05-26 — `spi_transceive()` returns ret=0 with rx=00 on EVT2 even
 * though the chip + bus + DMA all work (proven via bit-bang AND via
 * direct nrf_spim register writes — see evt_bringup/flash_test_evt2_raw_spim/
 * and DEBUGGING.md). The bug is in spi_nrfx_spim.c / nrfx_spim somewhere,
 * not in the silicon. So the flash JEDEC reads here bypass the Zephyr
 * SPI driver entirely and program SPIM3 via the HAL directly.
 *
 * The IMU path below (EVT1-only; EVT2 moved the IMU to I2C2) still
 * uses spi_transceive — it works on EVT1 with the Zephyr driver, no
 * need to touch it. The two functions cleanly disable SPIM3 on exit so
 * either path can be called in any order without state leaking.
 * ========================================================================
 */

#define FLASH_SCK_PIN    13   /* P0.13 */
#define FLASH_MISO_PIN   14   /* P0.14 */
#define FLASH_MOSI_PIN   15   /* P0.15 */
#define FLASH_CS_PIN     12   /* P0.12 */

/* Flash is happy with mode 0; LSM6DSO supports both mode 0 and mode 3.
 * A separate flag set per device so we can tune them independently if
 * the board's clock topology turns out to prefer one over the other.
 */
#define SPI_OP_MODE0 (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))
#define SPI_OP_MODE3 (SPI_OP_MODE0 | SPI_MODE_CPOL | SPI_MODE_CPHA)
#define SPI_OP       SPI_OP_MODE0

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));

static struct spi_config imu_cfg = {
	.frequency = 1000000,
	/* LSM6DSO supports both mode 0 and mode 3. Mode 3 was tried during
	 * the WHO_AM_I = 0x00 debugging in step 8; it didn't change the
	 * outcome (still 0x00 across two boards), so reverted to mode 0 to
	 * keep the IMU and the flash on the same bus polarity. The IMU
	 * failure is now classified as a board-level hardware issue (see
	 * steps.md step 12), not an SPI mode issue.
	 */
	.operation = SPI_OP_MODE0,
	.cs = {
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = 16,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};

/* (Was flash_cfg for spi_transceive(); the flash path now uses raw
 * SPIM3 HAL — see flash_spim_setup() / flash_spim_xfer() below.
 * Removed to avoid an unused-variable warning.) */

bool spi_probe_ready(void)
{
	if (!device_is_ready(spi)) {
		LOG_ERR("spi3 not ready");
		return false;
	}
	return true;
}

#define W25Q128_JEDEC_MFG  0xEF
#define W25Q128_JEDEC_TYPE 0x40
#define W25Q128_JEDEC_CAP  0x18
#define W25Q128_CMD_RDP    0xAB  /* Release from Deep Power-Down */
#define W25Q128_CMD_JEDEC  0x9F

/* RAM-resident, 4-byte-aligned EasyDMA buffers. Static + aligned + in
 * the .bss section, so EasyDMA can read/write them safely regardless of
 * how the function is called. */
static uint8_t flash_tx_buf[4] __aligned(4);
static uint8_t flash_rx_buf[4] __aligned(4);

/* One-time setup: configure CS as a GPIO output (idle HIGH) and program
 * SPIM3 PSEL/FREQ/CONFIG. Called from spi_flash_jedec() — re-doing the
 * setup on every call is cheap (a few register writes) and means we
 * don't have to worry about anyone else putting SPIM3 in a different
 * state between calls. */
static void flash_spim_setup(void)
{
	NRF_SPIM_Type *spim = NRF_SPIM3;
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);

	/* Disable so PSEL changes are safe. */
	spim->ENABLE = 0;

	spim->PSEL.SCK  = FLASH_SCK_PIN;
	spim->PSEL.MOSI = FLASH_MOSI_PIN;
	spim->PSEL.MISO = FLASH_MISO_PIN;
	/* nRF91 SPIM has no PSEL.CSN — CS is GPIO-driven (above). */

	spim->FREQUENCY = NRF_SPIM_FREQ_1M;
	spim->CONFIG    = 0;   /* mode 0, MSB-first */

	spim->ENABLE = 7;
}

/* Fire one CS-bracketed transfer via raw SPIM3 EasyDMA. Returns 0 on
 * success, -ETIMEDOUT if EVENTS_END never fires within ~100 ms. */
static int flash_spim_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	NRF_SPIM_Type *spim = NRF_SPIM3;
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	/* Copy caller's tx into our static aligned buffer, EasyDMA hates
	 * stack pointers in some configurations and our raw test proved
	 * the static-aligned path works. */
	if (tx) {
		memcpy(flash_tx_buf, tx, len);
	} else {
		memset(flash_tx_buf, 0, len);
	}
	memset(flash_rx_buf, 0, len);

	spim->TXD.PTR    = (uint32_t)flash_tx_buf;
	spim->TXD.MAXCNT = len;
	spim->RXD.PTR    = (uint32_t)flash_rx_buf;
	spim->RXD.MAXCNT = len;

	spim->EVENTS_END     = 0;
	spim->EVENTS_STARTED = 0;
	spim->EVENTS_ENDTX   = 0;
	spim->EVENTS_ENDRX   = 0;

	gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
	k_busy_wait(5);

	int64_t t0 = k_uptime_get();
	spim->TASKS_START = 1;
	while (spim->EVENTS_END == 0) {
		if (k_uptime_get() - t0 > 100) {
			gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
			return -ETIMEDOUT;
		}
	}

	gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
	k_busy_wait(5);

	if (rx) {
		memcpy(rx, flash_rx_buf, len);
	}
	return 0;
}

static void flash_spim_teardown(void)
{
	/* Disable so subsequent code that uses spi_transceive() (e.g.
	 * the IMU path on EVT1) won't pick up partial state. */
	NRF_SPIM_Type *spim = NRF_SPIM3;
	spim->ENABLE = 0;
}

/* Send Release-from-Deep-Power-Down (0xAB) and wait tRES1 (3 μs per
 * datasheet, 10 μs to be safe). Idempotent — if the chip is already
 * out of DPD this is a no-op. Worth doing because at the W25Q128JV's
 * minimum supply voltage power-up timing can leave the chip in DPD. */
static int flash_wake_from_dpd(void)
{
	uint8_t cmd[1] = { W25Q128_CMD_RDP };
	int ret = flash_spim_xfer(cmd, NULL, 1);
	k_usleep(10);
	return ret;
}

void spi_flash_jedec(void)
{
	const int max_attempts = 3;
	uint8_t last_rx[3] = { 0xff, 0xff, 0xff };

	flash_spim_setup();
	(void)flash_wake_from_dpd();

	for (int attempt = 1; attempt <= max_attempts; attempt++) {
		uint8_t tx[4] = { W25Q128_CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };

		int ret = flash_spim_xfer(tx, rx, sizeof(tx));
		if (ret) {
			LOG_ERR("flash JEDEC attempt %d/%d: raw spim err %d",
				attempt, max_attempts, ret);
			k_msleep(10);
			continue;
		}

		last_rx[0] = rx[1];
		last_rx[1] = rx[2];
		last_rx[2] = rx[3];

		bool ok = rx[1] == W25Q128_JEDEC_MFG &&
			  rx[2] == W25Q128_JEDEC_TYPE &&
			  rx[3] == W25Q128_JEDEC_CAP;

		LOG_INF("flash JEDEC attempt %d/%d: %02x %02x %02x %s",
			attempt, max_attempts, rx[1], rx[2], rx[3],
			ok ? "(W25Q128JV PASS via raw SPIM3 HAL)"
			   : "(unexpected; will retry)");

		if (ok) {
			test_report("spi3_flash", TEST_PASS,
				    "JEDEC %02x %02x %02x", rx[1], rx[2], rx[3]);
			flash_spim_teardown();
			return;
		}
		k_msleep(10);
	}

	LOG_WRN("  -> flash JEDEC FAIL after %d attempts via raw SPIM3 HAL "
		"(chip + traces previously proven via bit-bang)",
		max_attempts);
	test_report("spi3_flash", TEST_FAIL,
		    "JEDEC %02x %02x %02x (want EF 40 18)",
		    last_rx[0], last_rx[1], last_rx[2]);
	flash_spim_teardown();
}

#define LSM6DSO_REG_WHO_AM_I 0x0F
#define LSM6DSO_WHO_AM_I_VAL 0x6C

static int spi_imu_read_reg(uint8_t reg, uint8_t *out)
{
	uint8_t tx[2] = { reg | 0x80, 0x00 };
	uint8_t rx[2] = { 0 };

	const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

	int ret = spi_transceive(spi, &imu_cfg, &txs, &rxs);

	if (ret == 0) {
		*out = rx[1];
	}
	return ret;
}

void spi_imu_whoami(void)
{
	uint8_t val = 0;
	int ret;

	/* Some ST sensors latch their interface choice on the first
	 * communication after VDD ramp — if the very first read happens
	 * before the chip has decided I²C-vs-SPI, MISO can stay 0. A throw-
	 * away first read forces the SPI selection; the second read is the
	 * one whose value we trust.
	 */
	(void)spi_imu_read_reg(LSM6DSO_REG_WHO_AM_I, &val);
	k_msleep(10);

	ret = spi_imu_read_reg(LSM6DSO_REG_WHO_AM_I, &val);
	if (ret) {
		LOG_ERR("imu WHO_AM_I read failed: %d", ret);
		test_report("spi3_imu", TEST_FAIL, "spi err %d", ret);
		return;
	}

	LOG_INF("imu WHO_AM_I (0x0F) = 0x%02x (expected 0x6C for LSM6DSO)", val);

	if (val == LSM6DSO_WHO_AM_I_VAL) {
		LOG_INF("  -> LSM6DSO detected: PASS");
		test_report("spi3_imu", TEST_PASS, "WHO_AM_I=0x%02x", val);
	} else {
		LOG_WRN("  -> unexpected WHO_AM_I: FAIL");
		test_report("spi3_imu", TEST_FAIL,
			    "WHO_AM_I=0x%02x (want 0x6C)", val);
	}
}
