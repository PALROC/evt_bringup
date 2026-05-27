/*
 * EVT2 SPIM3 RAW-REGISTER test.
 *
 * This app does NOT use the Zephyr SPI driver. It does not use nrfx_spim
 * either. It talks directly to the SPIM3 hardware via Nordic's nrf_spim
 * HAL — thin static-inline functions over register writes — to test
 * whether the silicon itself can be made to do a single SPI transfer.
 *
 * Sequence on boot:
 *   1. Bit-bang JEDEC read at fast speed via GPIO. Proves the chip is
 *      alive and the bus is healthy. (Already proven in the sibling
 *      flash_test_evt2_minimal app's sweep; we keep it here as a
 *      one-shot baseline.)
 *   2. Release the pins back to high-Z.
 *   3. Manually program SPIM3 registers:
 *        ENABLE = 0      (disable to allow PSEL changes)
 *        PSEL.SCK  = P0.13
 *        PSEL.MOSI = P0.15
 *        PSEL.MISO = P0.14
 *        PSEL.CSN  = disconnected (we drive CS as a GPIO below)
 *        FREQUENCY = M1 (1 MHz)
 *        CONFIG    = mode 0, MSB-first
 *        TXD.PTR / .MAXCNT  → tx_buf
 *        RXD.PTR / .MAXCNT  → rx_buf
 *        ENABLE = 7 (enable)
 *   4. Drive CS low via GPIO.
 *   5. Trigger TASKS_START.
 *   6. Poll EVENTS_END (and log key registers along the way).
 *   7. Drive CS high.
 *   8. Report rx_buf and the peripheral's final state registers.
 *
 * If the bit-bang gets EF 40 18 PASS and the raw SPIM gets 00 00 00
 * FAIL, the bug is at the Zephyr/nrfx/HAL layer or in the silicon's
 * EasyDMA path. The register dump at the end tells us which:
 *  - ENABLE  should read 7
 *  - PSEL.*  should read their pin numbers (bit 31 = 0)
 *  - EVENTS_END should be 1 (=transfer reported complete)
 *  - TXD.AMOUNT / RXD.AMOUNT should = 4 (bytes the EasyDMA moved)
 *
 * If TXD.AMOUNT = 4 but rx_buf is all zeros, EasyDMA "moved" the
 * bytes through the FIFO but no clocks actually came out. That is
 * the silent-fail fingerprint we're seeing.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_spim.h>

LOG_MODULE_REGISTER(raw_spim, LOG_LEVEL_INF);

#define FLASH_CS_PIN  12
#define SCK_PIN       13
#define MISO_PIN      14
#define MOSI_PIN      15

#define CMD_JEDEC     0x9F

/* Buffers must be in RAM (stack is RAM, but we put them static so we
 * can hand the bus master a known fixed address). 4-byte aligned for
 * nice EasyDMA behaviour. */
static uint8_t tx_buf[4] __aligned(4) = { CMD_JEDEC, 0x00, 0x00, 0x00 };
static uint8_t rx_buf[4] __aligned(4);

/* --- Bit-bang baseline (~1 MHz, no delay) ------------------------------ */

static uint8_t bb_xchg(const struct device *gpio0, uint8_t out)
{
	uint8_t in = 0;
	for (int bit = 7; bit >= 0; bit--) {
		gpio_pin_set(gpio0, MOSI_PIN, (out >> bit) & 1);
		gpio_pin_set(gpio0, SCK_PIN, 1);
		in |= (gpio_pin_get(gpio0, MISO_PIN) & 1) << bit;
		gpio_pin_set(gpio0, SCK_PIN, 0);
	}
	return in;
}

static void bitbang_baseline(const struct device *gpio0)
{
	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(gpio0, SCK_PIN,      GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MOSI_PIN,     GPIO_OUTPUT_LOW);
	gpio_pin_configure(gpio0, MISO_PIN,     GPIO_INPUT);
	k_msleep(50);

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
	LOG_INF("BIT-BANG baseline JEDEC %02x %02x %02x  %s",
		rx[1], rx[2], rx[3], ok ? "PASS" : "FAIL");
}

/* --- Raw SPIM3 register-level test ------------------------------------- */

static void log_spim_state(const char *when)
{
	NRF_SPIM_Type *spim = NRF_SPIM3;

	/* Snapshot all values FIRST so we have a coherent moment-in-time
	 * picture even if printk takes long enough that the peripheral
	 * state could change between reads. */
	uint32_t enable = spim->ENABLE;
	uint32_t psck   = spim->PSEL.SCK;
	uint32_t pmosi  = spim->PSEL.MOSI;
	uint32_t pmiso  = spim->PSEL.MISO;
	uint32_t freq   = spim->FREQUENCY;
	uint32_t config = spim->CONFIG;
	uint32_t txptr  = spim->TXD.PTR;
	uint32_t txmax  = spim->TXD.MAXCNT;
	uint32_t txamt  = spim->TXD.AMOUNT;
	uint32_t rxptr  = spim->RXD.PTR;
	uint32_t rxmax  = spim->RXD.MAXCNT;
	uint32_t rxamt  = spim->RXD.AMOUNT;
	uint32_t evend  = spim->EVENTS_END;
	uint32_t evsta  = spim->EVENTS_STARTED;
	uint32_t evetx  = spim->EVENTS_ENDTX;
	uint32_t everx  = spim->EVENTS_ENDRX;

	/* Use printk for the dump — was losing lines to the LOG queue.
	 * One small sleep between each line so RTT has time to drain. */
	printk(">>> --- SPIM3 state @ %s ---\n", when);
	k_msleep(20);
	printk(">>>   ENABLE         = 0x%08x  (7=enabled)\n", enable);
	k_msleep(20);
	printk(">>>   PSEL.SCK       = 0x%08x  (bit31=1=>disconnected)\n", psck);
	k_msleep(20);
	printk(">>>   PSEL.MOSI      = 0x%08x\n", pmosi);
	k_msleep(20);
	printk(">>>   PSEL.MISO      = 0x%08x\n", pmiso);
	k_msleep(20);
	printk(">>>   FREQUENCY      = 0x%08x  (0x10000000=1M)\n", freq);
	k_msleep(20);
	printk(">>>   CONFIG         = 0x%08x\n", config);
	k_msleep(20);
	printk(">>>   TXD.PTR=0x%08x MAXCNT=%u AMOUNT=%u\n", txptr, txmax, txamt);
	k_msleep(20);
	printk(">>>   RXD.PTR=0x%08x MAXCNT=%u AMOUNT=%u\n", rxptr, rxmax, rxamt);
	k_msleep(20);
	printk(">>>   EVENTS_END=%u  EVENTS_STARTED=%u\n", evend, evsta);
	k_msleep(20);
	printk(">>>   EVENTS_ENDTX=%u  EVENTS_ENDRX=%u\n", evetx, everx);
	k_msleep(20);
}

static void raw_spim_test(const struct device *gpio0)
{
	NRF_SPIM_Type *spim = NRF_SPIM3;

	LOG_INF("===== RAW SPIM3 register test (1 MHz, mode 0) =====");

	/* Set CS pin as GPIO output, idle HIGH. We drive CS ourselves so
	 * we can keep PSEL.CSN disconnected (matches how the Zephyr
	 * cs-gpios mechanism would do it). */
	gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);

	/* Step 1: disable SPIM3 so we can safely change PSEL. */
	spim->ENABLE = 0;

	/* Step 2: pin assignment via PSEL (port 0 = bit 5 unset = value 0,
	 * just write the pin number directly; bit 31 = 1 means
	 * disconnected). */
	/* nRF91 SPIM has no peripheral-driven CSN pin (that's an nRF53/54
	 * feature). CS is always GPIO-driven, which is what we want here. */
	spim->PSEL.SCK  = SCK_PIN;             /* P0.13 */
	spim->PSEL.MOSI = MOSI_PIN;            /* P0.15 */
	spim->PSEL.MISO = MISO_PIN;            /* P0.14 */

	/* Step 3: frequency 1 MHz, mode 0, MSB-first. */
	spim->FREQUENCY = NRF_SPIM_FREQ_1M;
	spim->CONFIG    = (NRF_SPIM_MODE_0 | (NRF_SPIM_BIT_ORDER_MSB_FIRST << 0));

	/* Step 4: EasyDMA buffer setup. */
	memset(rx_buf, 0xAA, sizeof(rx_buf));  /* poison so we know if it was overwritten */
	tx_buf[0] = CMD_JEDEC;
	tx_buf[1] = tx_buf[2] = tx_buf[3] = 0;

	spim->TXD.PTR    = (uint32_t)tx_buf;
	spim->TXD.MAXCNT = sizeof(tx_buf);
	spim->RXD.PTR    = (uint32_t)rx_buf;
	spim->RXD.MAXCNT = sizeof(rx_buf);

	/* Clear stale events. */
	spim->EVENTS_END     = 0;
	spim->EVENTS_STARTED = 0;
	spim->EVENTS_ENDTX   = 0;
	spim->EVENTS_ENDRX   = 0;

	/* Step 5: enable. */
	spim->ENABLE = 7;

	log_spim_state("post-config / pre-START");

	/* Step 6: assert CS, trigger START, wait for END. */
	gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
	k_busy_wait(5);

	int64_t t_start = k_uptime_get();
	spim->TASKS_START = 1;

	/* Poll EVENTS_END with a 100 ms timeout — should fire in <100 us
	 * at 1 MHz for 4 bytes. */
	int polls = 0;
	while (spim->EVENTS_END == 0) {
		if (k_uptime_get() - t_start > 100) {
			LOG_ERR("TIMEOUT waiting for EVENTS_END (%d polls)", polls);
			break;
		}
		polls++;
	}
	int64_t t_end = k_uptime_get();
	LOG_INF("EVENTS_END fired after %d polls / %lld ms",
		polls, t_end - t_start);

	gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
	k_busy_wait(5);

	log_spim_state("post-transfer");

	/* Step 7: report what came back. */
	LOG_INF("rx_buf raw: %02x %02x %02x %02x",
		rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

	bool ok = (rx_buf[1] == 0xEF && rx_buf[2] == 0x40);
	LOG_INF("RAW SPIM3 JEDEC = %02x %02x %02x   %s",
		rx_buf[1], rx_buf[2], rx_buf[3],
		ok ? "PASS — chip alive via raw SPIM3 silicon!" :
		(rx_buf[1] == 0 && rx_buf[2] == 0 && rx_buf[3] == 0)
		    ? "FAIL — silent SPIM3 failure persists even raw" :
		    "(unexpected pattern)");

	/* Disable so a subsequent run / re-test starts clean. */
	spim->ENABLE = 0;
}

int main(void)
{
	LOG_INF("==========================================");
	LOG_INF("BUILD-TAG: flash_test_evt2_raw_spim 2026-05-26");
	LOG_INF("EVT2: bit-bang baseline then RAW SPIM3 register test");
	LOG_INF("==========================================");

	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready");
		return -ENODEV;
	}

	bitbang_baseline(gpio0);

	LOG_INF("Settling 2 s before raw SPIM3 test...");
	k_msleep(2000);

	/* Release pins from GPIO so SPIM3's PSEL can take them. */
	gpio_pin_configure(gpio0, SCK_PIN,  GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, MOSI_PIN, GPIO_DISCONNECTED);
	gpio_pin_configure(gpio0, MISO_PIN, GPIO_DISCONNECTED);
	/* CS stays as GPIO output — we drive it ourselves. */
	k_msleep(50);

	raw_spim_test(gpio0);

	LOG_INF("--- done; idling ---");
	while (1) {
		k_msleep(10000);
	}
	return 0;
}
