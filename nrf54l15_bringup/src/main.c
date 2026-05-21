/*
 * Copyright (c) 2025 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>

#include "test_report.h"
#include "wifi_probe.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Wi-Fi (nRF7000) bring-up moved here from the 9151 side. The chip's
 * SPI is wired to the L15 on EVT1; the 9151 used to host the driver
 * as a workaround while the L15 wasn't yet running.
 *
 * Note: the nrf70 driver binds at boot regardless of whether we call
 * wifi_passive_scan() — DT node has status="okay" + sysbuild sets
 * SB_CONFIG_WIFI_NRF70=y — so BUCKEN + IOVDD-CTL come up either way.
 * For *fully cold* nRF7000, disable the driver in sysbuild.conf and
 * set the nrf70 dts node to status="disabled".
 */
#define WIFI_PROBE_TIMEOUT_S    30

/* ===== PER-BOARD TRACKING ============================================
 * Change BOARD_NUMBER to match the physical board you're flashing.
 * The number appears in the test summary block and in the L15-side
 * test_summary.md record so per-board comparisons are easy.
 * ====================================================================
 */
#define BOARD_NUMBER       3
#define FW_VERSION_STRING  "0.3.0-l15-rffe-enable"

/* The L15 is now the npm1300 PMIC master over i2c20 (SDA P1.04 / SCL
 * P1.05) — it owns the nRF7000 power rails. So we no longer park those
 * pins high-Z; the i2c20 driver drives them. (The 9151 is off this bus —
 * its i2c2 is disabled in the board DT.)
 *
 * RFFE_BLE_WIFI_ENABLE = P1.10 — gates the BGS12SN6E6 RF switch VDD via
 * the RV2C010UNT2L gate FET. Must be HIGH for the switch to be powered.
 */
#define RFFE_BLE_WIFI_ENABLE_PIN  10

static int l15_early_gpio_init(void)
{
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

	if (!device_is_ready(gpio1)) {
		LOG_ERR("gpio1 not ready, can't init early GPIOs");
		return -ENODEV;
	}
	(void)gpio_pin_configure(gpio1, RFFE_BLE_WIFI_ENABLE_PIN,
				 GPIO_OUTPUT_HIGH);
	LOG_INF("RFFE_BLE_WIFI_ENABLE (P1.10) driven HIGH — RF switch powered");
	return 0;
}
SYS_INIT(l15_early_gpio_init, POST_KERNEL, 50);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Verify the L15 is the PMIC master on i2c20: both the i2c controller
 * and the npm1300 MFD child device should be ready after driver init.
 * If they are, the regulator driver has applied the DT init-microvolt
 * (BUCK1 = BUCK2 = 3.3 V) — which is the fix for BUCK1 = 1.8 V (strap
 * default) that was breaking the nRF7000. Replaces the old "verify the
 * shared i2c pins are parked high-Z" check, which was correct when the
 * 9151 was the PMIC master but is the opposite of what we want now.
 */
static void test_pmic_master(void)
{
	const struct device *i2c20 = DEVICE_DT_GET(DT_NODELABEL(i2c20));
	const struct device *pmic  = DEVICE_DT_GET(DT_NODELABEL(npm1300_pmic));

	if (!device_is_ready(i2c20)) {
		test_report("pmic_master", TEST_FAIL, "i2c20 not ready");
		return;
	}
	if (!device_is_ready(pmic)) {
		test_report("pmic_master", TEST_FAIL,
			    "npm1300 device not ready — PMIC ACK on i2c?");
		return;
	}
	test_report("pmic_master", TEST_PASS,
		    "i2c20 + npm1300 up; expect BUCK1=BUCK2=3.3 V on multimeter");
}

int main(void)
{
	int err;

	LOG_INF("nRF54L15 palroc bring-up start  BOARD=%d  fw=%s  built=%s %s",
		BOARD_NUMBER, FW_VERSION_STRING, __DATE__, __TIME__);

	/* ===== SAFETY: recovery window BEFORE any power-pin drive ===========
	 * The diagnostic below drives the nRF7000 load-switch controls HIGH,
	 * which (if mis-wired or if it triggers a supply runaway) could wedge
	 * the board. This 15 s do-nothing window opens on EVERY boot, before
	 * anything touches those pins — so if a previous flash left the board
	 * in a bad state, you always have 15 s here to interrupt with
	 * `nrfjprog --recover` / `west flash --erase` before the drive runs
	 * again. Do not move any GPIO drive above this loop.
	 */
	for (int s = 15; s > 0; s--) {
		LOG_INF("SAFETY: %d s recovery window (reflash now if needed)...", s);
		k_sleep(K_SECONDS(1));
	}

	/* If we got here, the kernel finished init — the LFXO/K32SRC fix
	 * (CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y) survived another reboot.
	 * That's the chief "alive" signal for the L15 on this board.
	 */
	test_report("boot", TEST_PASS, "kernel init OK, K32SRC_RC fix held");

	test_pmic_master();

	/* Power-path confirmed: driving P1.00/P1.01 HIGH from the app draws
	 * ~120 mA, so the L15 pins do reach the nRF7000 load switches. The
	 * nrf70 driver owns these pins (bucken-gpios / iovdd-ctrl-gpios), so
	 * we let it manage power rather than forcing the pins from here. */

	LOG_INF("--- nRF7000 passive Wi-Fi scan ---");
	wifi_passive_scan(WIFI_PROBE_TIMEOUT_S);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		test_report("ble_init", TEST_FAIL, "bt_enable err %d", err);
		test_report("ble_advertise", TEST_SKIP, "bt_enable failed");
		test_report_summary(BOARD_NUMBER, FW_VERSION_STRING);
		return err;
	}
	LOG_INF("BT stack initialised");
	test_report("ble_init", TEST_PASS, "bt_enable OK");

	err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("bt_le_adv_start failed: %d", err);
		test_report("ble_advertise", TEST_FAIL,
			    "bt_le_adv_start err %d", err);
		test_report_summary(BOARD_NUMBER, FW_VERSION_STRING);
		return err;
	}
	LOG_INF("Advertising as \"%s\" (non-connectable)", CONFIG_BT_DEVICE_NAME);
	test_report("ble_advertise", TEST_PASS,
		    "non-connectable, name=\"%s\"", CONFIG_BT_DEVICE_NAME);

	/* Print the test summary first so it's at the top of the RTT log
	 * and easy to copy out, before the long-running advertising loop
	 * fills the log with heartbeats.
	 */
	test_report_summary(BOARD_NUMBER, FW_VERSION_STRING);

	int n = 0;

	while (1) {
		k_sleep(K_SECONDS(10));
		LOG_INF("alive t=%ds (still advertising)", ++n * 10);
	}
	return 0;
}
