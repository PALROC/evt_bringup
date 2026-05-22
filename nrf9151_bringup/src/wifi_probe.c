#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fatal.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_probe.h"
#include "test_report.h"

LOG_MODULE_REGISTER(wifi_probe, LOG_LEVEL_INF);

static int wifi_best_rssi;

/* Emergency power-down: drive nRF7000 BUCK_EN and IOVDD_CTL LOW so the
 * TCK106AG load switches open and the chip is fully unpowered. Used by
 * the fatal-error handler below and exposed via wifi_emergency_off()
 * for explicit calls from app code (e.g., on a button press). Idempotent
 * and ISR-safe — just toggles two GPIOs.
 *
 * Pins are the 9151's P0.28 (BUCK_EN) and P0.29 (IOVDD_CTL), the same
 * physical pins the nrf70 driver references via bucken-gpios /
 * iovdd-ctrl-gpios on &nrf70 in the nova 9151 board dtsi.
 */
#define NRF7000_BUCKEN_PIN   28
#define NRF7000_IOVDDCTL_PIN 29

void wifi_emergency_off(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	if (!device_is_ready(gpio0)) {
		return;
	}
	(void)gpio_pin_configure(gpio0, NRF7000_IOVDDCTL_PIN, GPIO_OUTPUT_LOW);
	(void)gpio_pin_configure(gpio0, NRF7000_BUCKEN_PIN,   GPIO_OUTPUT_LOW);
}

/* Zephyr fatal-error hook. Called BEFORE the fault dump prints, so the
 * sequence in any catastrophic firmware failure is:
 *   1. Fault occurs (hard fault, stack overflow, k_oops, watchdog timeout)
 *   2. THIS HANDLER → cuts nRF7000 power
 *   3. Normal Zephyr fault dump prints to RTT
 *   4. System halts (nRF7000 stays unpowered)
 *
 * Effect: a board-burning runaway condition like the one that hurt PCB #1
 * cannot leave the Wi-Fi chip drawing power while the SoC is dead.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	wifi_emergency_off();
	LOG_PANIC();
	LOG_ERR("FATAL (reason=%u) — nRF7000 power cut as a precaution",
		reason);
}

static struct net_mgmt_event_callback scan_cb;
static K_SEM_DEFINE(scan_done_sem, 0, 1);
static int scan_result_count;

static const char *security_str(enum wifi_security_type sec)
{
	switch (sec) {
	case WIFI_SECURITY_TYPE_NONE:    return "open";
	case WIFI_SECURITY_TYPE_PSK:     return "WPA2-PSK";
	case WIFI_SECURITY_TYPE_PSK_SHA256: return "WPA2-PSK-SHA256";
	case WIFI_SECURITY_TYPE_SAE:     return "WPA3-SAE";
	case WIFI_SECURITY_TYPE_WAPI:    return "WAPI";
	case WIFI_SECURITY_TYPE_EAP:     return "WPA2-EAP";
	case WIFI_SECURITY_TYPE_WEP:     return "WEP";
	case WIFI_SECURITY_TYPE_WPA_PSK: return "WPA-PSK";
	default:                         return "?";
	}
}

/* mgmt_event is uint64_t in NCS 3.3.0 / Zephyr 4.x — it was widened from
 * uint32_t to accommodate larger event-code namespaces. With uint32_t, the
 * NET_EVENT_WIFI_* macros overflow at compile time and case-comparison
 * narrows the value, causing the wrong cases to fire (or nothing at all).
 */
static void on_scan_event(struct net_mgmt_event_callback *cb,
			  uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_SCAN_RESULT) {
		const struct wifi_scan_result *r =
			(const struct wifi_scan_result *)cb->info;

		scan_result_count++;
		if (r->rssi > wifi_best_rssi) {
			wifi_best_rssi = r->rssi;
		}
		LOG_INF("  [%2d] %-32s ch=%-3d rssi=%4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %s",
			scan_result_count,
			r->ssid_length ? (const char *)r->ssid : "(hidden)",
			r->channel, r->rssi,
			r->mac[0], r->mac[1], r->mac[2],
			r->mac[3], r->mac[4], r->mac[5],
			security_str(r->security));
	} else if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;

		if (st && st->status) {
			LOG_WRN("scan completed with status=%d", st->status);
		}
		k_sem_give(&scan_done_sem);
	}
}

void wifi_passive_scan(int timeout_seconds)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("no Wi-Fi interface — driver bound? (CONFIG_WIFI_NRF70)");
		test_report("wifi", TEST_FAIL, "no Wi-Fi netif registered");
		return;
	}

	LOG_INF("Wi-Fi interface ready (ifindex %d), waiting for it to come up...",
		net_if_get_by_iface(iface));

	/* Wait for the netif to be administratively up. The first scan
	 * attempt right after boot returned -EINPROGRESS (-115) because the
	 * driver was still finishing its async startup. Polling
	 * net_if_is_admin_up() for up to 3 s lets the driver finish without
	 * us having to guess the right delay.
	 */
	for (int i = 0; i < 30; i++) {
		if (net_if_is_admin_up(iface)) {
			break;
		}
		k_msleep(100);
	}
	if (!net_if_is_admin_up(iface)) {
		LOG_WRN("netif still not admin-up after 3 s — proceeding anyway");
	}

	net_mgmt_init_event_callback(&scan_cb, on_scan_event,
				     NET_EVENT_WIFI_SCAN_RESULT |
				     NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&scan_cb);

	/* Antenna covers 2.4 GHz + 5 GHz, so request both bands.
	 * `bands` is a bitmask: BIT(WIFI_FREQ_BAND_2_4_GHZ) | BIT(...5_GHZ).
	 * Passive-only is mandatory on this board (nPM1300 BUCK current
	 * limit 250 mA — see steps.md step 9).
	 */
	struct wifi_scan_params params = {
		.scan_type = WIFI_SCAN_TYPE_PASSIVE,
		.bands = BIT(WIFI_FREQ_BAND_2_4_GHZ) |
			 BIT(WIFI_FREQ_BAND_5_GHZ),
		.dwell_time_passive = 100,  /* ms per channel */
	};

	scan_result_count = 0;
	wifi_best_rssi = -127;
	k_sem_reset(&scan_done_sem);

	int err = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params,
			   sizeof(params));
	if (err) {
		LOG_ERR("net_mgmt(SCAN) failed: %d (driver/PMIC issue? "
			"check BUCKEN/IOVDD-CTL on multimeter)", err);
		net_mgmt_del_event_callback(&scan_cb);
		test_report("wifi", TEST_FAIL,
			    "net_mgmt(SCAN) err %d", err);
		return;
	}

	int sem_err = k_sem_take(&scan_done_sem, K_SECONDS(timeout_seconds));

	net_mgmt_del_event_callback(&scan_cb);

	if (sem_err) {
		LOG_WRN("scan timed out after %d s, results so far: %d",
			timeout_seconds, scan_result_count);
		test_report("wifi", scan_result_count > 0 ? TEST_PASS : TEST_FAIL,
			    "timeout, %d APs", scan_result_count);
		return;
	}

	LOG_INF("scan complete: %d AP%s seen", scan_result_count,
		scan_result_count == 1 ? "" : "s");

	if (scan_result_count == 0) {
		LOG_WRN("no APs found — antenna routed to Wi-Fi path? "
			"(check RF switch VC / SW_CTRL0 / RV2C010UNT2L MOSFET)");
		test_report("wifi", TEST_FAIL, "0 APs found");
	} else {
		test_report("wifi", TEST_PASS,
			    "%d APs, best RSSI %d dBm",
			    scan_result_count, wifi_best_rssi);
	}
}
