/*
 * EVT2 SPI flash JEDEC + PERSISTENCE probe, driven from the nRF54L15.
 *
 * Why this exists:
 * The L15-side read of the EVT2 W25Q128JV proved the chip is alive
 * (JEDEC ef 40 18). This app extends that with a write/erase/read
 * cycle that's the canonical "is the flash actually working" test:
 * write a known marker, power-cycle, read it back. If the marker
 * survives, the flash is healthy end-to-end.
 *
 * Layout used (sector 0 only):
 *   address 0x000000, 8 bytes:
 *     [0..3] = 'P' 'A' 'L' 'R'   (magic — proves it's our write)
 *     [4..7] = uint32_le counter (increments every boot)
 *
 * Sector 0 = 4 kB, which we erase each boot and then write 8 bytes.
 * Endurance is 100 k cycles per sector (datasheet) so even 1 cycle per
 * minute for a year is fine here.
 *
 * Build (NS variant — matches the bring-up):
 *   cd evt_bringup/flash_test_l15_evt2
 *   west build -p -b nrf54l15_nova_evt2/nrf54l15/cpuapp/ns -d build \
 *       -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
 *   west flash -d build
 *
 * Pre-conditions (same as before): 9151 erased so it doesn't drive the
 * shared SPI3 bus while the L15 owns it.
 *
 * SPIM00 quirks (learned the hard way, see DEBUGGING.md):
 *   - Minimum frequency 1.016 MHz (128 MHz core, max prescaler 126).
 *   - TF-M owns the peripheral on /ns; release via CONFIG_NRF_SPIM00_SECURE=n
 *     and the GPIO port configs in prj.conf.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_test, LOG_LEVEL_INF);

#define FLASH_CS_PORT_NODE  DT_NODELABEL(gpio2)
#define FLASH_CS_PIN        5

#define SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi00));

static struct spi_config cfg_flash = {
	.frequency = 4000000, /* 4 MHz — safely in SPIM00's 1.016..32 MHz range */
	.operation = SPI_OP,
	.cs = {
		.gpio = {
			.port = DEVICE_DT_GET(FLASH_CS_PORT_NODE),
			.pin = FLASH_CS_PIN,
			.dt_flags = GPIO_ACTIVE_LOW,
		},
		.delay = 2,
	},
};

/* ---- W25Q command codes ---- */
#define CMD_JEDEC          0x9F
#define CMD_READ_STATUS1   0x05
#define CMD_WRITE_ENABLE   0x06
#define CMD_PAGE_PROGRAM   0x02
#define CMD_SECTOR_ERASE   0x20  /* 4 KB sector erase */
#define CMD_READ_DATA      0x03

#define STATUS_WIP_BIT     0x01  /* Write In Progress (busy) */

/* Persistence marker layout. */
#define MARKER_ADDR        0x000000
#define MARKER_LEN         8
#define MAGIC_P            'P'
#define MAGIC_A            'A'
#define MAGIC_L            'L'
#define MAGIC_R            'R'

/* ---- low-level SPI helpers ---- */

static int spi_tx(const uint8_t *buf, size_t len)
{
	const struct spi_buf tx_buf = { .buf = (void *)buf, .len = len };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
	return spi_write(spi, &cfg_flash, &txs);
}

static int spi_txrx(const uint8_t *tx, uint8_t *rx, size_t len)
{
	const struct spi_buf tx_buf = { .buf = (void *)tx, .len = len };
	const struct spi_buf rx_buf = { .buf = rx,         .len = len };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };
	return spi_transceive(spi, &cfg_flash, &txs, &rxs);
}

/* ---- flash command primitives ---- */

static int flash_read_status(uint8_t *status)
{
	uint8_t tx[2] = { CMD_READ_STATUS1, 0x00 };
	uint8_t rx[2] = { 0 };
	int ret = spi_txrx(tx, rx, sizeof(tx));
	if (ret == 0) {
		*status = rx[1];
	}
	return ret;
}

/* Poll the WIP bit until cleared or timeout. Uses k_msleep(2) instead of
 * k_msleep(1) to reduce SPI traffic during the busy-wait — back-to-back
 * 1-ms-spaced status reads during the 45 ms erase were the prior crash
 * trigger (something about the back-to-back txrx flow). */
static int flash_wait_ready(int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	int polls = 0;
	while (k_uptime_get() < deadline) {
		uint8_t status;
		int ret = flash_read_status(&status);
		if (ret) {
			printk("  [wait_ready] status-read err %d after %d polls\n",
			       ret, polls);
			return ret;
		}
		polls++;
		if ((status & STATUS_WIP_BIT) == 0) {
			printk("  [wait_ready] ready after %d polls\n", polls);
			return 0;
		}
		k_msleep(2);
	}
	printk("  [wait_ready] TIMEOUT after %d polls\n", polls);
	return -ETIMEDOUT;
}

static int flash_write_enable(void)
{
	uint8_t cmd = CMD_WRITE_ENABLE;
	return spi_tx(&cmd, 1);
}

static int flash_sector_erase(uint32_t addr)
{
	int ret = flash_write_enable();
	if (ret) {
		return ret;
	}
	uint8_t tx[4] = {
		CMD_SECTOR_ERASE,
		(addr >> 16) & 0xFF,
		(addr >> 8) & 0xFF,
		addr & 0xFF,
	};
	ret = spi_tx(tx, sizeof(tx));
	if (ret) {
		return ret;
	}
	/* W25Q128JV typical sector erase ~45 ms, max 400 ms — use 800 ms
	 * timeout for headroom. */
	return flash_wait_ready(800);
}

/* Big DMA buffers as static — keeps them out of the stack so the main
 * thread isn't carrying 1 KB of buffers as it descends into flash code.
 * Stack overflow during sector erase wait was the prior crash. */
static uint8_t s_tx_buf[4 + 256];
static uint8_t s_rx_buf[4 + 256];

static int flash_page_program(uint32_t addr, const uint8_t *data, size_t len)
{
	if (len > 256) {
		return -EINVAL;
	}
	int ret = flash_write_enable();
	if (ret) {
		return ret;
	}
	s_tx_buf[0] = CMD_PAGE_PROGRAM;
	s_tx_buf[1] = (addr >> 16) & 0xFF;
	s_tx_buf[2] = (addr >> 8) & 0xFF;
	s_tx_buf[3] = addr & 0xFF;
	memcpy(&s_tx_buf[4], data, len);
	ret = spi_tx(s_tx_buf, 4 + len);
	if (ret) {
		return ret;
	}
	/* Page program typical 0.4 ms, max 3 ms — 20 ms timeout. */
	return flash_wait_ready(20);
}

static int flash_read(uint32_t addr, uint8_t *data, size_t len)
{
	if (len > 256) {
		return -EINVAL;
	}
	memset(s_tx_buf, 0, 4 + len);
	memset(s_rx_buf, 0, 4 + len);
	s_tx_buf[0] = CMD_READ_DATA;
	s_tx_buf[1] = (addr >> 16) & 0xFF;
	s_tx_buf[2] = (addr >> 8) & 0xFF;
	s_tx_buf[3] = addr & 0xFF;
	int ret = spi_txrx(s_tx_buf, s_rx_buf, 4 + len);
	if (ret == 0) {
		memcpy(data, &s_rx_buf[4], len);
	}
	return ret;
}

/* ---- JEDEC quick check (gate before doing anything write-y) ---- */

static int flash_jedec_check(void)
{
	uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
	uint8_t rx[4] = { 0 };
	int ret = spi_txrx(tx, rx, sizeof(tx));
	if (ret) {
		LOG_ERR("JEDEC read failed: %d", ret);
		return ret;
	}
	LOG_INF("JEDEC: %02x %02x %02x", rx[1], rx[2], rx[3]);

	if (rx[1] == 0xEF && rx[2] == 0x40) {
		return 0;
	}
	if (rx[1] == 0x1C && rx[2] == 0x30) {
		return 0;
	}
	LOG_ERR("Unrecognised JEDEC — refusing to write to an unknown chip");
	return -ENODEV;
}

/* ---- main: persistence test ---- */

int main(void)
{
	LOG_INF("=================================================");
	LOG_INF("EVT2 flash PERSISTENCE test (via L15 spi00)");
	LOG_INF("  Marker addr=0x%06x, 8 bytes:  'P' 'A' 'L' 'R' + uint32 counter",
		MARKER_ADDR);
	LOG_INF("=================================================");

	if (!device_is_ready(spi)) {
		LOG_ERR("spi00 not ready");
		return -ENODEV;
	}
	k_msleep(50);

	if (flash_jedec_check() != 0) {
		LOG_ERR("Aborting — JEDEC did not match a known W25Q-family chip.");
		while (1) {
			k_msleep(10000);
		}
	}

	/* === STEP 1: read current marker === */
	uint8_t buf[MARKER_LEN] = { 0 };
	int ret = flash_read(MARKER_ADDR, buf, MARKER_LEN);
	if (ret) {
		LOG_ERR("flash_read failed: %d", ret);
		goto idle;
	}
	LOG_HEXDUMP_INF(buf, MARKER_LEN, "marker bytes read from flash:");

	bool found = (buf[0] == MAGIC_P && buf[1] == MAGIC_A &&
		      buf[2] == MAGIC_L && buf[3] == MAGIC_R);
	uint32_t prev_counter = (uint32_t)buf[4]
				| ((uint32_t)buf[5] << 8)
				| ((uint32_t)buf[6] << 16)
				| ((uint32_t)buf[7] << 24);

	if (found) {
		LOG_INF("*** PERSISTENCE CONFIRMED ***");
		LOG_INF("    Previous boot wrote counter=%u — it survived the reset!",
			prev_counter);
	} else {
		LOG_INF("No prior PALR marker on this chip (first run, or sector was");
		LOG_INF("freshly erased / all-FFs). Will write counter=1.");
		prev_counter = 0;
	}

	/* === STEP 2: erase sector 0 === */
	printk(">>> BREADCRUMB: about to call flash_sector_erase()\n");
	int64_t t0 = k_uptime_get();
	ret = flash_sector_erase(MARKER_ADDR);
	int64_t dt = k_uptime_get() - t0;
	printk(">>> BREADCRUMB: flash_sector_erase() returned ret=%d after %lld ms\n",
	       ret, dt);
	if (ret) {
		LOG_ERR("sector erase failed: %d (took %lld ms)", ret, dt);
		goto idle;
	}
	LOG_INF("Erase OK in %lld ms.", dt);

	/* Small breather between erase complete and the next op, in case the
	 * back-to-back transitions on the bus were the prior crash. */
	k_msleep(10);

	/* === STEP 3: write new marker === */
	uint32_t new_counter = prev_counter + 1;
	uint8_t write_buf[MARKER_LEN] = {
		MAGIC_P, MAGIC_A, MAGIC_L, MAGIC_R,
		(uint8_t)(new_counter      ),
		(uint8_t)(new_counter >>  8),
		(uint8_t)(new_counter >> 16),
		(uint8_t)(new_counter >> 24),
	};
	printk(">>> BREADCRUMB: about to call flash_page_program() counter=%u\n",
	       new_counter);
	t0 = k_uptime_get();
	ret = flash_page_program(MARKER_ADDR, write_buf, MARKER_LEN);
	dt = k_uptime_get() - t0;
	printk(">>> BREADCRUMB: flash_page_program() returned ret=%d after %lld ms\n",
	       ret, dt);
	if (ret) {
		LOG_ERR("page program failed: %d (took %lld ms)", ret, dt);
		goto idle;
	}
	LOG_INF("Write OK in %lld ms.", dt);

	k_msleep(10);

	/* === STEP 4: read back, verify === */
	printk(">>> BREADCRUMB: about to call flash_read() for verify\n");
	uint8_t verify[MARKER_LEN] = { 0 };
	ret = flash_read(MARKER_ADDR, verify, MARKER_LEN);
	printk(">>> BREADCRUMB: verify flash_read() returned ret=%d\n", ret);
	if (ret) {
		LOG_ERR("readback failed: %d", ret);
		goto idle;
	}
	LOG_HEXDUMP_INF(verify, MARKER_LEN, "readback bytes:");

	bool match = memcmp(write_buf, verify, MARKER_LEN) == 0;
	if (match) {
		LOG_INF("*** WRITE+READ MATCH ***  flash is FULLY functional");
		LOG_INF("Power-cycle (or press reset on the L15) and re-run this app.");
		LOG_INF("Next boot should print:");
		LOG_INF("  '*** PERSISTENCE CONFIRMED *** Previous boot wrote counter=%u'",
			new_counter);
	} else {
		LOG_ERR("MISMATCH — wrote 0x%02x%02x%02x%02x%02x%02x%02x%02x but read 0x%02x%02x%02x%02x%02x%02x%02x%02x",
			write_buf[0], write_buf[1], write_buf[2], write_buf[3],
			write_buf[4], write_buf[5], write_buf[6], write_buf[7],
			verify[0], verify[1], verify[2], verify[3],
			verify[4], verify[5], verify[6], verify[7]);
	}

idle:
	LOG_INF("--- persistence test done; now polling JEDEC every 5 s as");
	LOG_INF("--- a heartbeat. Reset/power-cycle to re-run persistence. ---");

	/* JEDEC heartbeat loop — keeps confirming the chip is responsive
	 * after the write/erase operations. If JEDEC ever stops returning
	 * a valid Winbond ID, something has gone wrong on the bus. */
	uint32_t iter = 0;
	while (1) {
		iter++;
		uint8_t tx[4] = { CMD_JEDEC, 0x00, 0x00, 0x00 };
		uint8_t rx[4] = { 0 };
		int jret = spi_txrx(tx, rx, sizeof(tx));
		if (jret) {
			LOG_ERR("heartbeat %u: spi err %d", iter, jret);
		} else {
			bool ok = (rx[1] == 0xEF && rx[2] == 0x40) ||
				  (rx[1] == 0x1C && rx[2] == 0x30);
			LOG_INF("heartbeat %u: JEDEC %02x %02x %02x  %s",
				iter, rx[1], rx[2], rx[3],
				ok ? "alive" : "(unexpected)");
		}
		k_msleep(5000);
	}
	return 0;
}
