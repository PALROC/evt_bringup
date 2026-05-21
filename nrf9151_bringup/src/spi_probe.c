#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "spi_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(spi_probe, LOG_LEVEL_INF);

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

static struct spi_config flash_cfg = {
	.frequency = 1000000,
	.operation = SPI_OP,
	.cs = {
		.gpio = {
			.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
			.pin = 12,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};

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

/* Send Release-from-Deep-Power-Down (0xAB) and wait tRES1 (3 μs per the
 * datasheet, 10 μs to be safe). Idempotent — if the chip is already in
 * standby this is a no-op for our purposes. Useful here because the
 * W25Q128JV is at its minimum supply voltage on this board (BUCK2 =
 * 2.7 V, datasheet floor) so power-up timing is marginal and an
 * occasional partial init can leave the chip in DPD.
 */
static int flash_wake_from_dpd(void)
{
	uint8_t cmd = W25Q128_CMD_RDP;
	const struct spi_buf tx_buf = { .buf = &cmd, .len = 1 };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };

	int ret = spi_write(spi, &flash_cfg, &txs);

	k_usleep(10);
	return ret;
}

void spi_flash_jedec(void)
{
	const int max_attempts = 3;
	uint8_t last_rx[3] = { 0xff, 0xff, 0xff };

	(void)flash_wake_from_dpd();

	for (int attempt = 1; attempt <= max_attempts; attempt++) {
		uint8_t tx[4] = { W25Q128_CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };
		const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
		const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
		const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
		const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };

		int ret = spi_transceive(spi, &flash_cfg, &txs, &rxs);

		if (ret) {
			LOG_ERR("flash JEDEC attempt %d/%d: spi err %d",
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
			ok ? "(W25Q128JV PASS)" : "(unexpected; will retry)");

		if (ok) {
			test_report("spi3_flash", TEST_PASS,
				    "JEDEC %02x %02x %02x", rx[1], rx[2], rx[3]);
			return;
		}

		k_msleep(10);
	}

	LOG_WRN("  -> flash JEDEC FAIL after %d attempts (suspect SPI3 "
		"bus HW: board#2=00s, board#1=17s; voltage exonerated at "
		"BUCK2=2.9V — see steps.md step 12)", max_attempts);
	test_report("spi3_flash", TEST_FAIL,
		    "JEDEC %02x %02x %02x (want EF 40 18)",
		    last_rx[0], last_rx[1], last_rx[2]);
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
