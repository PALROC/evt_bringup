/*
 * SPI flash JEDEC ID probe for the EVT2 board, driven from the nRF9151.
 *
 * This is the test we've been building up to all day. From the L15 side
 * we already proved the W25Q128JV on EVT2 is fully alive (JEDEC ef 40 18,
 * write/erase/persistence all working). The open question is whether the
 * nRF9151 can read the SAME chip correctly through its own SPI3 path.
 *
 * EVT2 9151 SPI3 pin map (palroc_nova/.../nrf9151_nova_evt2-pinctrl.dtsi):
 *     SCK  = P0.13
 *     MOSI = P0.15
 *     MISO = P0.14
 *     CS   = P0.12   (the W25Q128JV footprint)
 *
 * The L15 shares these same physical SPI3 nets. We do NOT touch the L15
 * nRESET (per user request: it stays at its 10 kOhm pull-up default).
 *
 * **CRITICAL PRECONDITION: the L15 must be ERASED before running this
 * test.** With no firmware on the L15, its GPIOs default to high-Z
 * inputs and don't drive the bus. If the L15 boots its own firmware
 * (especially anything that touches spi00), it will fight us on MISO
 * and we'll get garbage. Erase first:
 *     nrfjprog --eraseall -f NRF54L --snr <L15_serial>
 *
 * Build:
 *   cd evt_bringup/flash_test_evt2
 *   west build -p -b nrf9151_nova_evt2/nrf9151/ns -d build \
 *       -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
 *   west flash -d build
 *
 * ===========================================================
 * What each outcome means:
 *
 *   JEDEC ef 40 18 PASS  -> 9151's SPI3 path works. The "9151 can't
 *                           read the flash" theory was wrong all along
 *                           (probably L15 contention on the historical
 *                           runs). EVT2 is fully alive end-to-end.
 *
 *   00 00 00             -> MISO held low even with L15 erased -> there
 *                           is a real EVT2 board-level defect on the
 *                           9151's MISO path (P0.14 trace, pad, or 9151
 *                           internal). Scope MISO at the chip's DO pad
 *                           vs the 9151 pin to localise.
 *
 *   ee 00 40 / shifted   -> same as the DK saw with the EVT1 chip clip
 *                           test: 9151 reads data but with timing/bit-
 *                           shift corruption. SI issue on this trace.
 *
 *   ff ff ff             -> MISO floating high. Wiring/trace open.
 * ===========================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_test, LOG_LEVEL_INF);

#define FLASH_CS_PIN    12
/* GPIO pin numbers on port 0 for bit-bang fallback (match the SPI3
 * pinctrl). */
#define SCK_PIN         13
#define MISO_PIN        14
#define MOSI_PIN        15

#define SPI_OP (SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8))

/* Driving the SAME physical flash, but routed through SPIM1 silicon
 * via the boards/*.overlay in this app — diagnostic test of whether
 * SPIM1 works while SPIM3 silicon is dead. */
static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi1));

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
		case 0x17: return "Winbond W25Q64  (64 Mbit)";
		case 0x18: return "Winbond W25Q128 (128 Mbit) — the EVT2 part";
		default:   return "Winbond W25Q (unknown density)";
		}
	}
	if (mfg == JEDEC_MFG_EON && type == 0x30) {
		switch (cap) {
		case 0x17: return "EON EN25Q64  (64 Mbit, W25Q-compatible)";
		case 0x18: return "EON EN25Q128 (128 Mbit)";
		default:   return "EON EN25Q (unknown density)";
		}
	}
	if (mfg == JEDEC_MFG_GIGADEVICE && type == 0x65) {
		return "GigaDevice GD25 (unexpected here)";
	}
	if (mfg == 0x00 || mfg == 0xFF) {
		return "(no chip responding — L15 likely still contending, OR board damage)";
	}
	return "(unrecognised — likely SI corruption on 9151 MISO trace)";
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
	/* Diagnostic block uses ONLY printk — the LOG subsystem was eating
	 * messages mid-write on this app (truncating at em-dashes / ANSI
	 * resets). printk goes directly to RTT, no queue, no formatter. */
	printk("\n");
	printk(">>> BUILD-TAG: flash_test_evt2 SPI1-OVERLAY-2026-05-26\n");
	printk(">>> EVT2 SPI flash JEDEC probe via 9151\n");
	/* Print which SPI device the DT actually gave us. If the overlay
	 * was applied, this should be 'spi@9000' (SPIM1). If overlay was
	 * silently ignored, we'd see 'spi@b000' (SPIM3). */
	printk(">>> SPI device name from DT: %s\n", spi ? spi->name : "(null)");
	printk(">>> expected: spi@9000 = SPIM1, or spi@b000 = SPIM3\n");
	printk(">>> pins via overlay: SCK=P0.13 MOSI=P0.15 MISO=P0.14 CS=P0.%d\n",
	       FLASH_CS_PIN);
	printk(">>> L15 must be erased for this test to be meaningful\n");

	{
		const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
		if (!device_is_ready(gpio0)) {
			printk(">>> ERR: gpio0 not ready\n");
		} else {
			printk(">>> --- P0.14 (MISO) pin health probe ---\n");

			gpio_pin_configure(gpio0, 14, GPIO_INPUT | GPIO_PULL_UP);
			k_msleep(2);
			int v_pu = gpio_pin_get(gpio0, 14);
			printk(">>>   P0.14 PULL-UP   reads %d  %s\n", v_pu,
			       v_pu == 1 ? "(pin alive)" : "(STUCK LOW)");

			gpio_pin_configure(gpio0, 14,
					   GPIO_INPUT | GPIO_PULL_DOWN);
			k_msleep(2);
			int v_pd = gpio_pin_get(gpio0, 14);
			printk(">>>   P0.14 PULL-DOWN reads %d  %s\n", v_pd,
			       v_pd == 0 ? "(pull-down wins)" : "(STUCK HIGH)");

			gpio_pin_configure(gpio0, 14, GPIO_INPUT);
			k_msleep(2);
			int v_np = gpio_pin_get(gpio0, 14);
			printk(">>>   P0.14 NO PULL   reads %d (floating)\n",
			       v_np);

			gpio_pin_configure(gpio0, FLASH_CS_PIN,
					   GPIO_INPUT | GPIO_PULL_UP);
			k_msleep(2);
			int v_cs = gpio_pin_get(gpio0, FLASH_CS_PIN);
			printk(">>>   P0.%d (CS) PULL-UP reads %d  %s\n",
			       FLASH_CS_PIN, v_cs,
			       v_cs == 1 ? "(idles HIGH)" : "(stuck LOW)");

			printk(">>> --- done pin probe ---\n");

			/* === BIT-BANG SPI FALLBACK ===
			 *
			 * Drive SPI manually using the same four pins as plain
			 * GPIOs. If this reads a valid JEDEC, the 9151's SPI3
			 * peripheral is broken and bit-bang is the workaround.
			 * If THIS also reads 00 00 00, the failure is upstream
			 * of the peripheral (CS net, MOSI/SCK propagation, etc).
			 *
			 * Protocol: SPI mode 0, MSB first, ~slow (k_busy_wait(2)
			 * = ~125 kHz). The W25Q128JV is rated to DC, so any speed
			 * the chip's setup/hold can handle is fine.
			 */
			printk(">>> --- BIT-BANG JEDEC read (bypassing SPI3) ---\n");

			/* Idle states. */
			gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);
			gpio_pin_configure(gpio0, SCK_PIN,      GPIO_OUTPUT_LOW);
			gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_OUTPUT_LOW);
			gpio_pin_configure(gpio0, MISO_PIN,     GPIO_INPUT);
			k_msleep(2);

			/* Assert CS, send 0x9F, read 3 bytes. */
			gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
			k_busy_wait(5);

			uint8_t bb_rx[4] = { 0 };
			const uint8_t bb_tx[4] = { 0x9F, 0x00, 0x00, 0x00 };
			for (int byte = 0; byte < 4; byte++) {
				uint8_t out = bb_tx[byte];
				uint8_t in  = 0;
				for (int bit = 7; bit >= 0; bit--) {
					/* Set MOSI while SCK is low (setup). */
					gpio_pin_set(gpio0, MOSI_PIN,
						     (out >> bit) & 1);
					k_busy_wait(2);
					/* Rising edge: chip samples MOSI,
					 * starts driving MISO for this bit. */
					gpio_pin_set(gpio0, SCK_PIN, 1);
					k_busy_wait(2);
					/* Sample MISO. */
					in |= (gpio_pin_get(gpio0, MISO_PIN) & 1)
					      << bit;
					/* Falling edge. */
					gpio_pin_set(gpio0, SCK_PIN, 0);
					k_busy_wait(2);
				}
				bb_rx[byte] = in;
			}

			gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
			k_busy_wait(5);

			/* Use printk so this line CANNOT be dropped by the
			 * deferred log queue — it's the whole point of this
			 * test app. */
			const char *verdict =
				(bb_rx[1] == 0xEF && bb_rx[2] == 0x40)
				? "*** PASS - chip alive via bit-bang! SPI3 peripheral is the bug ***"
				: (bb_rx[1] == 0x00 && bb_rx[2] == 0x00 && bb_rx[3] == 0x00)
					? "(STILL 00 - failure is electrical, not SPI3 peripheral)"
					: "(other pattern - investigate)";
			printk(">>> BITBANG JEDEC: %02x %02x %02x   %s\n",
			       bb_rx[1], bb_rx[2], bb_rx[3], verdict);
			printk(">>> BITBANG all 4 bytes raw: %02x %02x %02x %02x\n",
			       bb_rx[0], bb_rx[1], bb_rx[2], bb_rx[3]);

			/* === BIT-BANG READ-DATA from address 0x000000 ===
			 * The L15 wrote 8 bytes here in flash_test_l15_evt2:
			 *   [0..3] = 'P' 'A' 'L' 'R'
			 *   [4..7] = uint32_le boot counter
			 * If we can read those bytes back from the 9151 via
			 * bit-bang, the chip is fully validated end-to-end
			 * from both MCUs.
			 *
			 * Protocol: send 0x03 (READ DATA) + 3-byte address
			 * (0x00 0x00 0x00) + clock out 8 dummy bytes to read
			 * the data. 12 total bytes shifted. */
			printk(">>> --- BIT-BANG READ marker @ 0x000000 ---\n");

			gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
			k_busy_wait(5);

			uint8_t marker[8] = { 0 };
			/* 4 cmd+addr bytes (out) then 8 data bytes (in). */
			const uint8_t cmd[4] = { 0x03, 0x00, 0x00, 0x00 };
			for (int byte = 0; byte < 4 + 8; byte++) {
				uint8_t out = byte < 4 ? cmd[byte] : 0x00;
				uint8_t in  = 0;
				for (int bit = 7; bit >= 0; bit--) {
					gpio_pin_set(gpio0, MOSI_PIN,
						     (out >> bit) & 1);
					k_busy_wait(2);
					gpio_pin_set(gpio0, SCK_PIN, 1);
					k_busy_wait(2);
					in |= (gpio_pin_get(gpio0, MISO_PIN) & 1)
					      << bit;
					gpio_pin_set(gpio0, SCK_PIN, 0);
					k_busy_wait(2);
				}
				if (byte >= 4) {
					marker[byte - 4] = in;
				}
			}

			gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
			k_busy_wait(5);

			printk(">>> marker bytes: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			       marker[0], marker[1], marker[2], marker[3],
			       marker[4], marker[5], marker[6], marker[7]);

			bool magic_ok = marker[0] == 'P' && marker[1] == 'A' &&
					marker[2] == 'L' && marker[3] == 'R';
			if (magic_ok) {
				uint32_t counter = (uint32_t)marker[4]
					| ((uint32_t)marker[5] << 8)
					| ((uint32_t)marker[6] << 16)
					| ((uint32_t)marker[7] << 24);
				printk(">>> *** PALR FOUND *** counter=%u "
				       "(written earlier by the L15 — chip "
				       "stores data correctly across MCUs!)\n",
				       counter);
			} else if (marker[0] == 0xFF && marker[1] == 0xFF) {
				printk(">>> sector is erased (all 0xFF) — no marker present\n");
			} else {
				printk(">>> no PALR magic; chip returned data but "
				       "not what we expected. Either the L15 "
				       "hasn't written yet or this sector was overwritten.\n");
			}

			printk(">>> end bit-bang test; handing pins back to SPI3 driver\n");
		}
	}

	if (!device_is_ready(spi)) {
		printk(">>> ERR: spi3 not ready\n");
		return -ENODEV;
	}

	k_msleep(50);

	/* === RELEASE PINS BACK TO SPI PERIPHERAL ===
	 *
	 * The GPIO probe + bit-bang section above called gpio_pin_configure
	 * on P0.12/13/14/15. On the nRF91, writing PIN_CNF[n] takes the
	 * pin away from any peripheral that claimed it via PSEL — even
	 * though SPIM's PSEL register still points at the pin, GPIO's
	 * PIN_CNF dominates.
	 *
	 * Zephyr's SPIM driver only applies pinctrl ONCE at init time.
	 * If we've hijacked the pins as GPIO, the driver won't auto-
	 * reclaim them on transfer. We must explicitly release them by
	 * setting PIN_CNF back to "disconnected" — then the peripheral's
	 * PSEL is what controls the pin.
	 *
	 * GPIO_DISCONNECTED clears DIR + INPUT + PULL bits, effectively
	 * releasing the pin so the SPIM peripheral can drive it again.
	 */
	{
		const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
		printk(">>> Releasing P0.12/13/14/15 from GPIO back to SPIM\n");
		k_msleep(50);
		gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_DISCONNECTED);
		gpio_pin_configure(gpio0, SCK_PIN,      GPIO_DISCONNECTED);
		gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_DISCONNECTED);
		gpio_pin_configure(gpio0, MISO_PIN,     GPIO_DISCONNECTED);
		k_msleep(20);
	}

	/* === SPIM3 REGISTER DUMP — read the hardware state directly ===
	 *
	 * After the SPI driver has initialized SPIM3, dump the key
	 * registers so we can see what the peripheral *thinks* it's
	 * configured as. Compares to what we asked for via spi_config.
	 *
	 * SPIM3 base on nRF91 = 0x4000B000 (NS) / 0x5000B000 (S). The
	 * /ns app should see it at the NS alias 0x4000B000.
	 *
	 * Key register offsets (from nRF9151 product spec, SPIM chapter):
	 *   0x500  ENABLE       (7=enabled, 0=disabled)
	 *   0x504  PSEL.SCK
	 *   0x508  PSEL.MOSI
	 *   0x50C  PSEL.MISO
	 *   0x510  PSEL.CSN
	 *   0x524  FREQUENCY
	 *   0x534  CONFIG       (CPOL/CPHA + ORDER)
	 *   0x540  ORC          (overrun character)
	 */
	/* We're testing SPIM1 now (not SPIM3) — different peripheral
	 * instance, different base. Same alias rules: NS in /ns build, S
	 * in secure build. */
#if defined(CONFIG_TRUSTED_EXECUTION_NONSECURE)
#define SPIM3_BASE  0x40009000U  /* SPIM1 NS alias */
#define SPIM3_LABEL "SPIM1 NS @ 0x40009000"
#else
#define SPIM3_BASE  0x50009000U  /* SPIM1 S alias */
#define SPIM3_LABEL "SPIM1 S  @ 0x50009000"
#endif

	{
		volatile uint32_t *spim3 = (volatile uint32_t *)SPIM3_BASE;

		/* SAFETY PROBE: dip a toe in the water — read just the ENABLE
		 * register and announce. If THIS hangs, the peripheral block
		 * is inaccessible from our security domain. */
		printk(">>> probe %s ...\n", SPIM3_LABEL);
		k_msleep(100);
		uint32_t probe = spim3[0x500 / 4];
		printk(">>> probe RETURNED 0x%08x  (chip-side: ENABLE)\n",
		       probe);
		k_msleep(100);

		/* Snapshot all values FIRST (atomic w.r.t. logging) before
		 * any printing — that way if a register read faults, we know
		 * exactly which one. Then print one per line with k_msleep
		 * between each so RTT has time to drain (previous attempts
		 * lost output to the deferred log queue). */
		uint32_t en      = spim3[0x500 / 4];
		uint32_t psck    = spim3[0x504 / 4];
		uint32_t pmosi   = spim3[0x508 / 4];
		uint32_t pmiso   = spim3[0x50C / 4];
		uint32_t pcsn    = spim3[0x510 / 4];
		uint32_t freq    = spim3[0x524 / 4];
		uint32_t cfg     = spim3[0x534 / 4];
		uint32_t orc     = spim3[0x540 / 4];

		printk(">>> SPIM3 reg dump @ 0x4000B000\n");
		k_msleep(50);
		printk(">>>   ENABLE    = 0x%08x\n", en);
		k_msleep(50);
		printk(">>>   PSEL.SCK  = 0x%08x\n", psck);
		k_msleep(50);
		printk(">>>   PSEL.MOSI = 0x%08x\n", pmosi);
		k_msleep(50);
		printk(">>>   PSEL.MISO = 0x%08x\n", pmiso);
		k_msleep(50);
		printk(">>>   PSEL.CSN  = 0x%08x\n", pcsn);
		k_msleep(50);
		printk(">>>   FREQUENCY = 0x%08x\n", freq);
		k_msleep(50);
		printk(">>>   CONFIG    = 0x%08x\n", cfg);
		k_msleep(50);
		printk(">>>   ORC       = 0x%08x\n", orc);
		k_msleep(50);
		printk(">>> (ENABLE: 7=on, 0=off | PSEL bit31=1 means disconnected\n");
		k_msleep(50);
		printk(">>>  FREQ: 0x80000000=1M, 0x40000000=8M | CONFIG bits: 0=ORDER 1=CPHA 2=CPOL)\n");
		k_msleep(50);
	}

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
			printk(">>> iter %u: spi err %d\n", iter, ret);
			k_msleep(5000);
			continue;
		}

		uint8_t mfg = rx[1], type = rx[2], cap = rx[3];
		bool ok = (mfg == JEDEC_MFG_WINBOND && type == 0x40) ||
			  (mfg == JEDEC_MFG_EON     && type == 0x30);

		printk(">>> iter %u: JEDEC %02x %02x %02x  %s\n",
		       iter, mfg, type, cap, ok ? "PASS" : "----");

		k_msleep(5000);
	}

	return 0;
}
