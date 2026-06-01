/*
 * Reliability test — periodic CoAP heartbeats over LTE-M.
 *
 * Flow:
 *   boot
 *     -> telemetry_init() + bump_boot_count()
 *     -> nrf_modem_lib_init()
 *     -> modem_info_init()
 *     -> lte_lc_connect()                  (LTE-M attach, ~5 s)
 *     -> coap_client_connect()             (UDP socket open, DNS resolve)
 *     -> loop forever:
 *           telemetry_collect()
 *           telemetry_encode_cbor()
 *           coap_client_post_heartbeat()
 *           on success: sleep COAP_HEARTBEAT_INTERVAL_S
 *           on failure: short retry, then reconnect from scratch
 *
 * The modem PSM settings in prj.conf let the modem deep-sleep between
 * heartbeats while keeping its EPS attachment, so wake-up cost is
 * single seconds instead of a full attach cycle.
 *
 * Failure modes handled (defensively, not exhaustively):
 *   - LTE link drop -> lte_lc reattach on next iteration
 *   - DNS / server unreachable -> reset socket, retry next interval
 *   - CoAP non-2.xx response -> retry next interval (no payload loss
 *     since we don't queue anything; v1 is best-effort)
 *
 * Things we intentionally DON'T do in v1:
 *   - Queue heartbeats while offline (NVS-backed queue would be v2)
 *   - DTLS-CID (v2, once the unencrypted loop is proven)
 *   - Adaptive cadence by signal strength (v2)
 *   - GNSS fix in the payload (v2 if useful)
 */

#include "telemetry.h"
#include "coap_client.h"
#include "server_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define MAX_PAYLOAD_BYTES   256
#define FAIL_BACKOFF_MS     30000   /* on send failure, retry sooner than the full interval */
#define LED_BLINK_MS        1000    /* red LED on for 1 s per heartbeat */

static volatile bool lte_connected;

/* --- Red LED blink on heartbeat send --------------------------------
 *
 * We use a delayable work-item so the LED-off happens asynchronously
 * without blocking the main loop's sendto/recv path. The LED stays
 * on for LED_BLINK_MS then turns itself off.
 *
 * If the heartbeat takes longer than LED_BLINK_MS to complete (e.g.
 * slow ACK), the LED just goes off as scheduled — that's fine, the
 * "I sent a packet" signal already fired. */
static const struct gpio_dt_spec led_red =
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(led_red), gpios, {0});

static void led_off_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(led_off_work, led_off_work_fn);

static void led_off_work_fn(struct k_work *w)
{
	(void)gpio_pin_set_dt(&led_red, 0);
}

static void led_blip(void)
{
	if (!led_red.port) {
		return;     /* DT didn't declare led-red alias */
	}
	(void)gpio_pin_set_dt(&led_red, 1);
	(void)k_work_reschedule(&led_off_work, K_MSEC(LED_BLINK_MS));
}

static void led_init(void)
{
	if (!led_red.port) {
		LOG_WRN("no led-red alias in DT; LED blink disabled");
		return;
	}
	if (!device_is_ready(led_red.port)) {
		LOG_WRN("led-red GPIO port not ready");
		return;
	}
	(void)gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
}

static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			lte_connected = true;
			LOG_INF("LTE registered (status=%d)", evt->nw_reg_status);
		} else {
			lte_connected = false;
			LOG_WRN("LTE NOT registered (status=%d)", evt->nw_reg_status);
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("PSM update: TAU=%d s  Active=%d s",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"connected" : "idle");
		break;
	default:
		break;
	}
}

static int wait_for_lte(int timeout_s)
{
	for (int i = 0; i < timeout_s; i++) {
		if (lte_connected) {
			return 0;
		}
		k_msleep(1000);
	}
	return -ETIMEDOUT;
}

int main(void)
{
	int err;

	LOG_INF("==========================================");
	LOG_INF("RELIABILITY TEST — periodic CoAP heartbeats");
	LOG_INF("  server: %s:%d  path=/%s  interval=%d s",
		COAP_SERVER_HOST, COAP_SERVER_PORT,
		COAP_HEARTBEAT_PATH, COAP_HEARTBEAT_INTERVAL_S);
	LOG_INF("==========================================");

	/* --- LED init (red blinks for 1 s on every successful send) --- */
	led_init();

	/* --- Telemetry init (chip ID + reset reason + boot count) --- */
	telemetry_init();
	telemetry_bump_boot_count();

	/* --- Modem init ----------------------------------------------- */
	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed: %d — rebooting in 30 s", err);
		k_msleep(30000);
		sys_reboot(SYS_REBOOT_COLD);
	}

	err = modem_info_init();
	if (err) {
		LOG_WRN("modem_info_init failed: %d (RSRP/RAT may be blank)", err);
	}

	/* --- LTE-M attach -------------------------------------------- */
	/* lte_lc_init() is auto-called by lte_lc_connect() in NCS 3.x —
	 * the standalone init function was removed. Just register the
	 * event handler then call connect(). */
	lte_lc_register_handler(lte_evt_handler);

	LOG_INF("requesting LTE-M attach...");
	err = lte_lc_connect();
	if (err) {
		LOG_ERR("lte_lc_connect failed: %d", err);
	}

	err = wait_for_lte(90);
	if (err) {
		LOG_ERR("LTE attach timed out after 90 s — rebooting");
		k_msleep(5000);
		sys_reboot(SYS_REBOOT_COLD);
	}

	/* --- Open the CoAP UDP socket -------------------------------- */
	err = coap_client_connect();
	if (err) {
		LOG_ERR("coap_client_connect failed: %d — will retry in loop", err);
	}

	/* --- Heartbeat loop ------------------------------------------ */
	uint32_t iter = 0;
	while (1) {
		iter++;
		LOG_INF("--- heartbeat #%u ---", iter);

		telemetry_t t;
		telemetry_collect(&t);
		LOG_INF("  uptime=%us  boot=%u  rst=%u",
			t.uptime_s, t.boot_count, t.reset_reason);
		LOG_INF("  battery: vbat=%dmV  ibat=%+dmA  ntc=%d°C",
			t.vbat_mV, t.ibat_mA, t.ntc_C);
		LOG_INF("  cellular: rat=%s  rsrp=%ddBm  modem=%d°C",
			t.rat[0] ? t.rat : "?", t.rsrp_dBm, t.modem_C);
		if (t.imu_ok) {
			LOG_INF("  imu: ax=%+dmg ay=%+dmg az=%+dmg  imu=%d°C",
				t.accel_x_mg, t.accel_y_mg, t.accel_z_mg, t.imu_C);
		} else {
			LOG_WRN("  imu: read failed");
		}

		uint8_t payload[MAX_PAYLOAD_BYTES];
		int n = telemetry_encode_cbor(&t, payload, sizeof(payload));
		if (n < 0) {
			LOG_ERR("CBOR encode failed: %d", n);
			k_msleep(FAIL_BACKOFF_MS);
			continue;
		}
		LOG_INF("  cbor payload %d bytes", n);

		led_blip();   /* visible "transmitting now" indicator */
		err = coap_client_post_heartbeat(payload, (size_t)n);
		if (err) {
			LOG_WRN("heartbeat #%u FAILED (err %d) — backoff %d ms",
				iter, err, FAIL_BACKOFF_MS);
			k_msleep(FAIL_BACKOFF_MS);
			continue;
		}
		LOG_INF("heartbeat #%u OK — sleeping %d s",
			iter, COAP_HEARTBEAT_INTERVAL_S);

		k_msleep(COAP_HEARTBEAT_INTERVAL_S * 1000);
	}

	return 0;
}
