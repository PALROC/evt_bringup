/*
 * Copyright (c) 2025 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/bluetooth/bluetooth.h>

#include "test_report.h"
#include "uart_chat.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* BLE name = "palroc-l15-<6 hex digits from chip ID>". Built once at
 * boot from the L15 SoC's factory-programmed unique device ID, so it's
 * stable across reboots and unique per board. Used as the advertising
 * NAME_COMPLETE field AND echoed back to the 9151 in the BLE_READY
 * reply so the demo log shows the exact name the phone scanner will
 * see. */
#define BLE_NAME_PREFIX "palroc-l15-"
static char ble_name[sizeof(BLE_NAME_PREFIX) + 6];
static bool ble_started;

static void compute_ble_name(void)
{
	uint8_t id[16];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	if (n < 3) {
		/* Fallback: deterministic-looking placeholder so the demo
		 * still has a complete name to advertise. */
		snprintf(ble_name, sizeof(ble_name), BLE_NAME_PREFIX "000000");
		LOG_WRN("hwinfo unavailable (n=%d) — using fallback BLE name",
			(int)n);
		return;
	}
	snprintf(ble_name, sizeof(ble_name), BLE_NAME_PREFIX "%02x%02x%02x",
		 id[n - 3], id[n - 2], id[n - 1]);
}

/* Bring BLE up (bt_enable + bt_le_adv_start) with the chip-ID-derived
 * name. Idempotent — second + later calls just no-op.
 *
 * BLE is auto-started from main() at boot. The old (working) bring-up
 * did this too, and an active BLE radio on the L15 turns out to be
 * needed for the 9151's nRF7000 Wi-Fi scan to find any APs (the shared
 * antenna switch / RF subsystem path apparently depends on it).
 * The BLE_START message from the 9151 doesn't actually start BLE
 * (already up) — it's the "tell me your advertised name" trigger that
 * the on_uart_line() handler responds to with BLE_READY <name>. */
static void ensure_ble_advertising(void)
{
	if (ble_started) {
		return;
	}

	int err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		test_report("ble_init", TEST_FAIL, "bt_enable err %d", err);
		return;
	}
	LOG_INF("BT stack initialised");
	test_report("ble_init", TEST_PASS, "bt_enable OK");

	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS,
			      (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA(BT_DATA_NAME_COMPLETE, ble_name, strlen(ble_name)),
	};

	err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("bt_le_adv_start failed: %d", err);
		test_report("ble_advertise", TEST_FAIL,
			    "bt_le_adv_start err %d", err);
		return;
	}

	LOG_INF("Advertising as \"%s\" (non-connectable)", ble_name);
	test_report("ble_advertise", TEST_PASS, "name=\"%s\"", ble_name);
	ble_started = true;
}

/* RX callback. Runs on the system workqueue (not in ISR context), so
 * synchronous bt_enable / advertising start is fine here. */
static void on_uart_line(const char *line)
{
	LOG_INF("[uart] <- 9151: \"%s\"", line);

	if (strcmp(line, "BLE_START") == 0) {
		/* Ensure BLE is up (idempotent — usually already up from
		 * the boot-time auto-start), then announce the name back
		 * to the 9151 so the demo log shows the trigger landed and
		 * the advertised name is current. */
		char reply[UART_CHAT_LINE_MAX];
		ensure_ble_advertising();
		if (ble_started) {
			snprintf(reply, sizeof(reply), "BLE_READY %s",
				 ble_name);
			uart_chat_send(reply);
		} else {
			uart_chat_send("BLE_FAIL adv_start");
		}
	} else if (strcmp(line, "HELLO_9151") == 0) {
		/* Link smoke test from commit 2 still answered — handy for
		 * debugging the link in isolation without triggering BLE. */
		uart_chat_send("HELLO_L15");
	}
}

/* Wi-Fi (nRF7000) host = the 9151 on EVT1. See
 * evt_bringup/nrf9151_bringup/src/wifi_probe.c. The L15 doesn't run
 * the Wi-Fi stack here. */

/* ===== PER-BOARD TRACKING ============================================
 * Change BOARD_NUMBER to match the physical board you're flashing.
 * The number appears in the test summary block and in the L15-side
 * test_summary.md record so per-board comparisons are easy.
 * ====================================================================
 */
#define BOARD_NUMBER       3
#define FW_VERSION_STRING  "0.3.0-l15-rffe-enable"

/* Shared i²c2 bus with the 9151:
 *   L15 P1.04 ↔ SDA ↔ 9151 P0.09
 *   L15 P1.05 ↔ SCL ↔ 9151 P0.08
 * The 9151 is the bus master. To make sure the L15 doesn't disturb
 * 9151-side i²c traffic, force these pins to high-Z input with no pull
 * at boot — *before* anything else runs. The L15 will be observable on
 * the bus only as a passive line; if we ever want it as a master or
 * slave, we'd configure i²c here instead.
 *
 * Inter-MCU bus arbitration on the shared SPI3 + i²c2 buses is application
 * firmware's design problem (per step 16); this bring-up's job is just
 * proving the L15 chip itself is alive.
 */
#define L15_I2C_SHARED_SDA_PIN    4
#define L15_I2C_SHARED_SCL_PIN    5
/* RFFE_BLE_WIFI_ENABLE = P1.10 — gates the BGS12SN6E6 RF switch VDD via
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
	(void)gpio_pin_configure(gpio1, L15_I2C_SHARED_SDA_PIN,
				 GPIO_INPUT | GPIO_DISCONNECTED);
	(void)gpio_pin_configure(gpio1, L15_I2C_SHARED_SCL_PIN,
				 GPIO_INPUT | GPIO_DISCONNECTED);
	LOG_INF("shared i²c pins (P1.4 SDA, P1.5 SCL) parked as high-Z inputs");
	(void)gpio_pin_configure(gpio1, RFFE_BLE_WIFI_ENABLE_PIN,
				 GPIO_OUTPUT_HIGH);
	LOG_INF("RFFE_BLE_WIFI_ENABLE (P1.10) driven HIGH — RF switch powered");
	return 0;
}
SYS_INIT(l15_early_gpio_init, POST_KERNEL, 50);

/* Verify the SYS_INIT-parked i²c pins really did go high-Z and aren't
 * being held by anything else (a bus partner pulling, a stray short, etc.).
 * Reads the pin level back; with a 9151-side pull-up at idle the line
 * should read 1, with no pull-up it floats and may read 0 or 1.
 * Pass = the pin is configurable as input + read returned without error.
 */
static void test_gpio_park(void)
{
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

	if (!device_is_ready(gpio1)) {
		test_report("gpio_park", TEST_FAIL, "gpio1 not ready");
		return;
	}
	int sda = gpio_pin_get(gpio1, L15_I2C_SHARED_SDA_PIN);
	int scl = gpio_pin_get(gpio1, L15_I2C_SHARED_SCL_PIN);

	if (sda < 0 || scl < 0) {
		test_report("gpio_park", TEST_FAIL,
			    "read err sda=%d scl=%d", sda, scl);
		return;
	}
	test_report("gpio_park", TEST_PASS,
		    "P1.4(SDA)=%d  P1.5(SCL)=%d  (high-Z, levels passive)",
		    sda, scl);
}

int main(void)
{
	LOG_INF("nRF54L15 palroc bring-up start  BOARD=%d  fw=%s  built=%s %s",
		BOARD_NUMBER, FW_VERSION_STRING, __DATE__, __TIME__);

	/* If we got here, the kernel finished init — the LFXO/K32SRC fix
	 * (CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y) survived another reboot.
	 * That's the chief "alive" signal for the L15 on this board. */
	test_report("boot", TEST_PASS, "kernel init OK, K32SRC_RC fix held");

	test_gpio_park();

	compute_ble_name();
	LOG_INF("BLE name: %s", ble_name);
	test_report("ble_name", TEST_PASS, "%s", ble_name);

	/* Bring up the inter-MCU link. */
	if (uart_chat_init(on_uart_line) == 0) {
		test_report("uart_link", TEST_PASS, "dut-uart up");
	} else {
		test_report("uart_link", TEST_FAIL, "dut-uart not ready");
	}

	/* Bring BLE up NOW (not deferred to the BLE_START handshake from the
	 * 9151) — the old (working) bring-up did this, and the 9151's
	 * nRF7000 Wi-Fi scan turns out to depend on the L15's BLE radio
	 * being active (shared antenna switch / RF state). The BLE_START
	 * message still triggers a BLE_READY reply, but advertising is up
	 * the whole time. */
	ensure_ble_advertising();

	test_report_summary(BOARD_NUMBER, FW_VERSION_STRING);

	int n = 0;
	while (1) {
		k_sleep(K_SECONDS(10));
		LOG_INF("alive t=%ds (BLE %s)", ++n * 10,
			ble_started ? "advertising" : "waiting for BLE_START");
	}
	return 0;
}
