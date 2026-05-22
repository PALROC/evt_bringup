/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "diag.h"
#include "leds.h"
#include "i2c_probe.h"
#include "npm1300.h"
#include "spi_probe.h"
#include "modem.h"
#include "gnss.h"
#include "wifi_probe.h"
#include "uart_chat.h"
#include "button_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Long initial pause AFTER the reset-reason print, so a host reconnecting
 * RTT after a silent reset has plenty of time to attach and capture the
 * cause line before any further activity buries it.
 */
#define POST_RESET_HOLD_MS 5000
#define BOOT_SETTLE_MS     500
#define INTER_PHASE_MS     100

/* Set to 1 to run a multi-band sweep instead of the single-band
 * low-power attach. The sweep iterates the bands[] array at the top of
 * modem_band_sweep() in modem.c, with a 90 s timeout per band, and logs
 * which ones attach + signal quality on each.
 */
#define RUN_BAND_SWEEP 0

/* Set to 1 to run the GNSS probe (COEX0 LNA gate + CFUN=31 + continuous
 * tracking + per-5s satellite/CN0/fix log) right after modem_probe() and
 * BEFORE the LTE attach/sweep, with the modem returned to CFUN=0 at the
 * end so the LTE phase starts clean. Default 0 because indoor first fix
 * can take minutes; turn on when validating the antenna/LNA path or with
 * line-of-sight to the sky.
 */
#define RUN_GNSS_PROBE       0
#define GNSS_PROBE_DURATION  300

/* SPI3 bus contention test: the nRF54L15 shares SPI3 lines (SCK/MOSI/MISO)
 * with the W25Q128JV flash and LSM6DSO IMU on this board. If the L15 is
 * booting (10 kΩ pull-up on its nRESET keeps it out of reset by default)
 * and its GPIOs do anything to those lines, we'd see exactly the corrupt-
 * but-deterministic SPI reads we observed (board#2=00s, board#1=17s).
 * Setting this to 1 drives 9151 P0.19 LOW, holding the L15 in reset and
 * leaving its GPIOs high-Z, so SPI3 becomes uncontested.
 *
 * Default 1 while debugging the SPI3 issue. Flip to 0 to bring the L15
 * back up (its 10 kΩ pull-up will hold nRESET high once we stop driving
 * P0.19 low).
 */
/* Set to 1 to drive P0.19 LOW (overriding the 10 kΩ pull-up on the L15
 * nRESET) so the L15 stays in reset and its GPIOs go high-Z. Used for
 * SPI3 bus contention debugging — leave at 0 once you actually want the
 * L15 to boot.
 */
#define HOLD_L15_IN_RESET 0
#define L15_NRESET_PIN    19

/* Set to 1 to run a single passive Wi-Fi scan via the nRF7000 on SPI1.
 * Logs every AP found (SSID / BSSID / channel / RSSI / security). The
 * driver handles BUCKEN + IOVDD-CTL + firmware patch upload internally;
 * the bring-up just needs a wifi_mgmt scan request. Passive-only is
 * enforced in the scan params because of the BUCK current limit
 * (steps.md step 9 / project_palroc_nrf7000_supply.md).
 *
 * Antenna note: the same antenna feeds both nRF7000 (Wi-Fi) and nRF54L15
 * (BLE) through a BGS12SN6E6 SPDT switch gated by SW_CTRL0 → RV2C010UNT2L.
 * If SW_CTRL0 is high-Z (because the L15 is in reset), the switch may not
 * route the antenna to the Wi-Fi path and we'll see zero APs even with a
 * healthy chip. To be resolved before expecting real scan output.
 */
/* Set to 1 to run a single passive Wi-Fi scan via the nRF7000 on SPI1.
 * Logs every AP found (SSID / BSSID / channel / RSSI / security). The
 * driver handles BUCKEN + IOVDD-CTL + firmware patch upload internally;
 * the bring-up just needs a wifi_mgmt scan request. Passive-only is
 * enforced in scan params (BUCK current limit, see steps.md step 9). */
#define RUN_WIFI_PROBE        1
#define WIFI_PROBE_TIMEOUT_S  30

/* Set to 1 to run the hall-sensor button probe on P0.01 BEFORE the Wi-Fi
 * scan. Logs every transition + 1 Hz heartbeat showing the current level,
 * mirrors level to the red LED for live visual feedback while you wave
 * a magnet over the sensor. Useful for understanding what the sensor
 * actually does (active-high vs active-low, single transition per pass
 * vs multiple, latch behaviour, etc.).
 */
#define RUN_BUTTON_PROBE        0
#define BUTTON_PROBE_DURATION   60

/* ===== PER-BOARD TRACKING ============================================
 * Change BOARD_NUMBER for each physical board you flash. The number
 * appears in the test summary block at the end of every run, so per-
 * board comparisons are easy.
 *
 * Bump FW_VERSION_STRING when a meaningful firmware change happens
 * (new probe added, fix in a probe, etc.). The build timestamp from
 * __DATE__/__TIME__ disambiguates two builds with the same version
 * string on the same board.
 * ====================================================================
 */
#define BOARD_NUMBER       4
#define FW_VERSION_STRING  "0.15.0-step16-testreport"

/* Gate the whole probe sequence behind a Hall-2 press so the demo starts
 * on a deliberate trigger. Hall-2 is on P0.01, active-LOW (pressed = 0).
 * Polls at 50 ms intervals; the pin is configured as input with internal
 * pull-up (safe whether the sensor is open-drain or push-pull). */
#define HALL2_PIN              1
#define HALL2_POLL_INTERVAL_MS 50

static void wait_for_hall2_press(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	if (!device_is_ready(gpio0)) {
		LOG_ERR("gpio0 not ready, cannot read Hall 2 — running anyway");
		return;
	}
	(void)gpio_pin_configure(gpio0, HALL2_PIN, GPIO_INPUT | GPIO_PULL_UP);

	LOG_INF("waiting for Hall 2 (P0.%d) press to start the demo...",
		HALL2_PIN);
	uint32_t ticks = 0;
	while (gpio_pin_get(gpio0, HALL2_PIN) != 0) {
		k_msleep(HALL2_POLL_INTERVAL_MS);
		ticks++;
		if (ticks % (1000 / HALL2_POLL_INTERVAL_MS) == 0) {
			LOG_INF("  still waiting on Hall 2 (t=%u s)...",
				ticks / (1000 / HALL2_POLL_INTERVAL_MS));
		}
	}
	LOG_INF("Hall 2 pressed — starting demo");
}

/* Smoke-test callback for the inter-MCU link. Commit 3 will replace
 * this with the actual demo state machine (BLE start request etc). */
static void on_uart_line(const char *line)
{
	LOG_INF("[uart] <- L15: \"%s\"", line);
}

int main(void)
{
	LOG_INF("nRF9151 bring-up start  BOARD=%d  fw=%s  built=%s %s",
		BOARD_NUMBER, FW_VERSION_STRING, __DATE__, __TIME__);
	diag_print_reset_reason();

#if HOLD_L15_IN_RESET
	{
		const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

		if (device_is_ready(gpio0)) {
			gpio_pin_configure(gpio0, L15_NRESET_PIN,
					   GPIO_OUTPUT_LOW);
			LOG_INF("nRF54L15 held in reset (P0.%d driven LOW) "
				"to isolate SPI3 bus", L15_NRESET_PIN);
		} else {
			LOG_ERR("gpio0 not ready, can't hold L15 in reset");
		}
	}
#endif

	/* Hold here so RTT viewers reconnecting after a silent reset can
	 * grab the cause line above before everything else moves on.
	 */
	LOG_INF("post-reset hold: %d ms (reconnect RTT now if needed)...",
		POST_RESET_HOLD_MS);
	k_msleep(POST_RESET_HOLD_MS);

	boot_indicator();
	k_msleep(BOOT_SETTLE_MS);

	/* Bring the inter-MCU link up at boot so the L15 is ready to receive
	 * the instant we send. We don't send anything yet — the actual
	 * exchange is triggered by the Hall 2 press below, so the RTT
	 * timeline reads as: silent boot -> Hall press -> handshake. */
	(void)uart_chat_init(on_uart_line);

	wait_for_hall2_press();

	/* Link smoke test, deliberately gated by Hall 2 so it shows up in
	 * the demo log right after the trigger. Commit 3 replaces this with
	 * the real BLE-start handshake. */
	if (uart_chat_ready()) {
		uart_chat_send("HELLO_9151");
	}

	LOG_INF("--- Modem (init + AT only) ---");
	modem_probe();
	k_msleep(INTER_PHASE_MS);

#if RUN_GNSS_PROBE
	gnss_probe(GNSS_PROBE_DURATION);
	k_msleep(INTER_PHASE_MS);
#endif

	/* CFUN=4 soak passed (step 5.1 — see steps.md). Now try a low-power
	 * attach: B8 band-lock + 1 dB TX back-off, software-only mitigations
	 * to see if the supply can carry the reduced peak currents.
	 *
	 * To re-run the CFUN=4 isolation soak, swap the call below for:
	 *   modem_cfun4_soak(60);
	 * To run the unrestricted attach, swap for:
	 *   modem_lte_attach();
	 */
#if RUN_BAND_SWEEP
	LOG_INF("--- LTE band sweep ---");
	modem_band_sweep();
#else
	LOG_INF("--- LTE attach (NB-IoT, low-power, single band) ---");
	modem_lte_attach_lowpower();
#endif
	k_msleep(INTER_PHASE_MS);

	LOG_INF("--- I2C2 scan ---");
	i2c_scan();
	k_msleep(INTER_PHASE_MS);

	LOG_INF("--- nPM1300 read (VBAT / NTC / IBAT) ---");
	/* Wait until the 15 s boot mark before reading. The PMIC's ADC and
	 * gauge state can need a moment to settle after power-on; reading
	 * too early gave us a dropped/garbled sample line on the first
	 * attempt. 15 s is plenty of headroom and synchronises the read
	 * with PPK2 captures.
	 */
	{
		const int64_t target_ms = 15000;
		int64_t now = k_uptime_get();

		if (now < target_ms) {
			LOG_INF("waiting %lld ms until t=15s for PMIC settle...",
				target_ms - now);
			k_msleep(target_ms - now);
		}
	}
	npm1300_probe();
	k_msleep(INTER_PHASE_MS);

	LOG_INF("--- SPI3 flash @ CS=P0.12 ---");
	spi_flash_jedec();
	k_msleep(INTER_PHASE_MS);

	/* LSM6DSO IMU. EVT2 moved it off SPI3 onto i2c2 — pick the probe by
	 * whether the board DT has an `lsm6dso` node (present on EVT2, absent
	 * on EVT1 where the IMU is a raw-SPI device with no DT child). */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(lsm6dso), okay)
	LOG_INF("--- I2C2 IMU @ 0x6a (EVT2) ---");
	i2c_imu_whoami();
	/* Real-data smoke test: read one accelerometer sample and log it
	 * in m/s². With the board flat you should see ~9.81 on Z (or
	 * whichever axis is vertical for your orientation). */
	i2c_imu_accel_read();
#else
	LOG_INF("--- SPI3 IMU @ CS=P0.16 (EVT1) ---");
	spi_imu_whoami();
#endif
	k_msleep(INTER_PHASE_MS);

#if RUN_BUTTON_PROBE
	button_probe(BUTTON_PROBE_DURATION);
	k_msleep(INTER_PHASE_MS);
#endif

#if RUN_WIFI_PROBE
	LOG_INF("--- nRF7000 passive Wi-Fi scan ---");
	wifi_passive_scan(WIFI_PROBE_TIMEOUT_S);
	k_msleep(INTER_PHASE_MS);
#endif

	LOG_INF("--- LEDs walk ---");
	leds_walk();

#if !RUN_BAND_SWEEP
	/* Tag SKIP for any test that wasn't actually run this build, so the
	 * summary table makes the build-time configuration obvious rather
	 * than silently omitting them.
	 */
	test_report("band_sweep", TEST_SKIP, "RUN_BAND_SWEEP=0");
#endif
#if !RUN_GNSS_PROBE
	test_report("gnss", TEST_SKIP, "RUN_GNSS_PROBE=0");
#endif
#if !RUN_BUTTON_PROBE
	test_report("hall2", TEST_SKIP, "RUN_BUTTON_PROBE=0");
	test_report("hall1", TEST_SKIP, "RUN_BUTTON_PROBE=0");
#endif
#if !RUN_WIFI_PROBE
	test_report("wifi", TEST_SKIP, "RUN_WIFI_PROBE=0");
#endif

	LOG_INF("bring-up complete");
	test_report_summary(BOARD_NUMBER, FW_VERSION_STRING);
	return 0;
}
