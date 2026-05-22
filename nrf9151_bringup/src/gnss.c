#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_agnss.h>
#include <net/nrf_cloud_rest.h>

#include "gnss.h"
#include "test_report.h"

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

/* Latest PVT frame published by the modem GNSS event callback.
 * Read by the periodic-status loop in gnss_probe(). The struct contains
 * a satellite array sv[] and a flags byte indicating fix validity.
 */
static struct nrf_modem_gnss_pvt_data_frame latest_pvt;
static atomic_t pvt_count = ATOMIC_INIT(0);

/* A-GNSS assistance request raised by the modem (Stage C). Captured in
 * the GNSS event handler when NRF_MODEM_GNSS_EVT_AGNSS_REQ fires, then
 * serviced from the probe loop (the CoAP fetch is blocking, so it must
 * not run in the event-handler context). */
static struct nrf_modem_gnss_agnss_data_frame agnss_req;
static atomic_t agnss_req_pending = ATOMIC_INIT(0);

static void gnss_event_handler(int event)
{
	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		if (nrf_modem_gnss_read(&latest_pvt, sizeof(latest_pvt),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			atomic_inc(&pvt_count);
		}
		break;
	case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
		if (nrf_modem_gnss_read(&agnss_req, sizeof(agnss_req),
					NRF_MODEM_GNSS_DATA_AGNSS_REQ) == 0) {
			atomic_set(&agnss_req_pending, 1);
		}
		break;
	default:
		break;
	}
}

/* Count satellites the receiver is currently aware of, and return the
 * highest CN0 (in 0.1 dB-Hz units) seen across them. `tracked_out` is
 * the slot count with a non-zero SV ID — these are the satellites the
 * receiver is actively decoding. Even one non-zero CN0 means signal is
 * reaching the antenna, which is what we need to validate the LNA path.
 */
static void summarise_pvt(int *tracked_out, uint16_t *max_cn0_out,
			  int *used_in_fix_out)
{
	int tracked = 0;
	int used = 0;
	uint16_t max_cn0 = 0;

	for (int i = 0; i < (int)ARRAY_SIZE(latest_pvt.sv); i++) {
		if (latest_pvt.sv[i].sv == 0) {
			continue;
		}
		tracked++;
		if (latest_pvt.sv[i].cn0 > max_cn0) {
			max_cn0 = latest_pvt.sv[i].cn0;
		}
		if (latest_pvt.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
			used++;
		}
	}

	*tracked_out = tracked;
	*max_cn0_out = max_cn0;
	*used_in_fix_out = used;
}

/* Per-satellite dump. Iterate sv[] and print every entry with a non-zero
 * SV ID: signal strength (CN0 in dB-Hz), elevation/azimuth (degrees), and
 * the two informative flags ("used" = used in current fix, "unhealthy" =
 * SV broadcast says not to use). High CN0 with stable IDs across logs is
 * the signal we actually want — the LNA path is delivering RF and the
 * receiver is tracking healthy birds.
 */
static void log_satellites(void)
{
	for (int i = 0; i < (int)ARRAY_SIZE(latest_pvt.sv); i++) {
		if (latest_pvt.sv[i].sv == 0) {
			continue;
		}
		bool used = (latest_pvt.sv[i].flags &
			     NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) != 0;
		bool unhealthy = (latest_pvt.sv[i].flags &
				  NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY) != 0;

		LOG_INF("      SV %3u  cn0=%2u.%u dB-Hz  el=%3d  az=%3d  %s%s",
			latest_pvt.sv[i].sv,
			latest_pvt.sv[i].cn0 / 10,
			latest_pvt.sv[i].cn0 % 10,
			latest_pvt.sv[i].elevation,
			latest_pvt.sv[i].azimuth,
			used ? "[used] " : "       ",
			unhealthy ? "[UNHEALTHY]" : "");
	}
}

/* Print lat/lon as integer degrees + microdegrees, avoiding %f / cbprintf
 * floating-point support (which would inflate flash for one log line).
 */
static void log_fix_position(void)
{
	int32_t lat_deg = (int32_t)latest_pvt.latitude;
	int32_t lat_udeg = (int32_t)((latest_pvt.latitude - lat_deg) * 1000000.0);
	if (lat_udeg < 0) {
		lat_udeg = -lat_udeg;
	}

	int32_t lon_deg = (int32_t)latest_pvt.longitude;
	int32_t lon_udeg = (int32_t)((latest_pvt.longitude - lon_deg) * 1000000.0);
	if (lon_udeg < 0) {
		lon_udeg = -lon_udeg;
	}

	int32_t alt_m = (int32_t)latest_pvt.altitude;
	int32_t acc_m = (int32_t)latest_pvt.accuracy;

	LOG_INF("    lat=%d.%06d lon=%d.%06d alt=%dm acc=%dm",
		lat_deg, lat_udeg, lon_deg, lon_udeg, alt_m, acc_m);
}

void gnss_probe(int duration_seconds)
{
	char resp[160];
	int err;

	LOG_INF("--- GNSS probe (%d s) ---", duration_seconds);

	/* GNSS needs GPS active in the system mode. The dual-RAT LTE test
	 * (modem_lte_attach_both) leaves the modem in NB-IoT with no GPS, so
	 * on a demo loop iteration CFUN=31 would fail with CME 65536 ("no
	 * GNSS subsystem to wake"). Force a GPS-capable mode here. It can
	 * only be changed while non-active, so drop to CFUN=0 first; the
	 * later LTE test re-sets the mode to what it needs.
	 */
	nrf_modem_at_printf("AT+CFUN=0");
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("system_mode_set(GPS) failed: %d", err);
		test_report("gnss", TEST_FAIL, "GPS mode set err %d", err);
		return;
	}

	/* COEX0 → external GNSS LNA enable. The palroc board uses COEX0 to
	 * gate the LNA powered from nPM1300 LDO1; the modem drives this
	 * line high during GNSS RX so the LNA is only powered when needed.
	 *
	 * AT%XCOEX0=<mode>,<gnss_pin>,<f_low_MHz>,<f_high_MHz>
	 *   mode=1: enable COEX0
	 *   gnss_pin=1: drive high during GNSS RX
	 *   1565..1586 MHz: GPS L1 / GLONASS L1 band
	 */
	/* gnss_pin: 1 = COEX0 driven HIGH during GNSS RX (active-high LNA
	 * enable, the most common topology). Flip to 0 if your LNA enable is
	 * active-LOW or the schematic inverts COEX0 between modem and LNA —
	 * symptom of a polarity mismatch is zero SVs tracked outdoors with
	 * clear sky AND the AT%XCOEX0? readback confirming the params we
	 * wrote: in that case the AT side is doing exactly what we asked,
	 * but "what we asked" is the opposite of what the LNA needs.
	 */
	err = nrf_modem_at_cmd(resp, sizeof(resp),
			       "AT%%XCOEX0=1,1,1565,1586");
	if (err) {
		LOG_ERR("AT%%XCOEX0 failed: %d (raw: %s)", err, resp);
		/* Continue anyway — without COEX0 the LNA stays off and
		 * tracking will be much weaker, but GNSS still functions. */
	} else {
		LOG_INF("COEX0 → GNSS L1 LNA enabled (1565-1586 MHz)");
	}

	/* Read back what the modem actually has set. If the params don't
	 * match what we wrote, the modem firmware uses a different syntax
	 * for AT%XCOEX0 in this version (some mfw revs want extra WCDMA
	 * params), and we need to adjust the set command above.
	 */
	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XCOEX0?") == 0) {
		LOG_INF("  COEX0 readback: %s", resp);
	} else {
		LOG_WRN("  AT%%XCOEX0? readback failed (mfw may not support query)");
	}

	/* CFUN=31 = GNSS only, LTE off. Cleanest GNSS test: no LTE TX
	 * competing for the antenna front-end, no risk of TX-vs-RX brownout
	 * during the test, and we don't need a SIM/network for any of this.
	 */
	err = nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=31");
	if (err) {
		LOG_ERR("AT+CFUN=31 failed: %d (raw: %s)", err, resp);
		return;
	}
	LOG_INF("CFUN=31 (GNSS only) set");

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_gnss_event_handler_set failed: %d", err);
		return;
	}

	/* Continuous tracking: 1 s fix interval, no retry deadline. */
	(void)nrf_modem_gnss_fix_interval_set(1);
	(void)nrf_modem_gnss_fix_retry_set(0);

	atomic_set(&pvt_count, 0);
	memset(&latest_pvt, 0, sizeof(latest_pvt));

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("nrf_modem_gnss_start failed: %d", err);
		return;
	}
	LOG_INF("GNSS started, tracking for %d s...", duration_seconds);

	atomic_val_t last_count = 0;
	uint16_t best_cn0_seen = 0;
	int max_tracked = 0;

	for (int t = 5; t <= duration_seconds; t += 5) {
		k_msleep(5000);

		atomic_val_t now_count = atomic_get(&pvt_count);
		long new_pvts = now_count - last_count;
		last_count = now_count;

		int tracked, used_in_fix;
		uint16_t max_cn0;

		summarise_pvt(&tracked, &max_cn0, &used_in_fix);
		if (tracked > max_tracked) {
			max_tracked = tracked;
		}
		if (max_cn0 > best_cn0_seen) {
			best_cn0_seen = max_cn0;
		}

		bool fix = (latest_pvt.flags &
			    NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) != 0;

		LOG_INF("  t=%3ds  PVTs=%ld (+%ld)  tracked=%d  in_fix=%d  "
			"max_cn0=%u.%u dB-Hz  fix=%s",
			t, (long)now_count, new_pvts, tracked, used_in_fix,
			max_cn0 / 10, max_cn0 % 10, fix ? "YES" : "no");

		if (tracked > 0) {
			log_satellites();
		}
		if (fix) {
			log_fix_position();
		}
	}

	err = nrf_modem_gnss_stop();
	if (err) {
		LOG_ERR("nrf_modem_gnss_stop failed: %d", err);
	}

	/* Park modem in CFUN=0 so a subsequent step (LTE attach, band sweep)
	 * starts from a known-clean state.
	 */
	(void)nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=0");

	LOG_INF("--- GNSS probe complete: %ld total PVTs, max_tracked=%d, "
		"best_cn0=%u.%u dB-Hz ---",
		(long)atomic_get(&pvt_count), max_tracked,
		best_cn0_seen / 10, best_cn0_seen % 10);

	/* GNSS reports as INFO rather than PASS/FAIL because outdoor sky-view
	 * is required for any meaningful result, and the bring-up criterion
	 * is "the path is alive enough to see SVs," not "we got a fix indoors."
	 * Inspect the detail string in the summary table to judge per-board.
	 */
	bool got_fix = (latest_pvt.flags &
			NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) != 0;
	test_report("gnss", TEST_INFO,
		    "max_SV=%d  best_cn0=%u.%u dB-Hz  fix=%s",
		    max_tracked, best_cn0_seen / 10, best_cn0_seen % 10,
		    got_fix ? "YES" : "no");
}

/* Fetch the A-GNSS assistance the modem asked for from nRF Cloud over
 * CoAP and inject it into the modem. Assumes nRF Cloud CoAP is already
 * connected. Buffer holds one assistance response (~3.5 KB is the
 * typical full set; sized with headroom). */
static void agnss_fetch_and_process(void)
{
	/* 2 KB was too small even with filtered ephemerides ("data cannot fit
	 * in result buffer", -105). 4 KB is the standard A-GNSS response size;
	 * fits in RAM now that the provisioning lib is out of this build. */
	static char agnss_buf[4096];
	struct nrf_cloud_rest_agnss_request request = {
		.type = NRF_CLOUD_REST_AGNSS_REQ_CUSTOM,
		.agnss_req = &agnss_req,
	};
	struct nrf_cloud_rest_agnss_result result = {
		.buf = agnss_buf,
		.buf_sz = sizeof(agnss_buf),
	};
	int err;

	LOG_INF("A-GNSS: requesting assistance from nRF Cloud...");
	err = nrf_cloud_coap_agnss_data_get(&request, &result);
	if (err) {
		LOG_ERR("A-GNSS: data_get failed: %d", err);
		test_report("agnss", TEST_FAIL, "data_get err %d", err);
		return;
	}

	err = nrf_cloud_agnss_process(result.buf, result.agnss_sz);
	if (err) {
		LOG_ERR("A-GNSS: process failed: %d", err);
		test_report("agnss", TEST_FAIL, "process err %d", err);
		return;
	}

	LOG_INF("A-GNSS: injected %zu bytes of assistance into the modem",
		result.agnss_sz);
	test_report("agnss", TEST_PASS, "injected %zu bytes", result.agnss_sz);
}

void gnss_probe_assisted(int duration_seconds)
{
	char resp[160];
	int err;

	LOG_INF("--- GNSS probe (assisted, %d s) ---", duration_seconds);

	/* A-GNSS needs LTE up to fetch assistance, so use LTE-M + GPS in the
	 * system mode (modem time-shares the two) and CFUN=1 — NOT the
	 * offline CFUN=31 path the unassisted gnss_probe() uses. */
	nrf_modem_at_printf("AT+CFUN=0");
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("system_mode_set(LTEM_GPS) failed: %d", err);
		test_report("gnss", TEST_FAIL, "mode set err %d", err);
		return;
	}

	LOG_INF("attaching LTE for A-GNSS...");
	err = lte_lc_connect();
	if (err) {
		LOG_ERR("LTE attach failed: %d", err);
		test_report("gnss", TEST_FAIL, "LTE attach err %d", err);
		return;
	}

	/* Connect to nRF Cloud over CoAP using the provisioned credentials. */
	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init failed: %d", err);
		test_report("gnss", TEST_FAIL, "coap_init err %d", err);
		return;
	}
	LOG_INF("connecting to nRF Cloud (CoAP)...");
	err = nrf_cloud_coap_connect(NULL);
	if (err) {
		LOG_ERR("nrf_cloud_coap_connect failed: %d (provisioned?)", err);
		test_report("gnss", TEST_FAIL, "coap_connect err %d", err);
		return;
	}
	LOG_INF("nRF Cloud connected");

	/* COEX0 -> external GNSS LNA enable (same as the offline path). */
	if (nrf_modem_at_cmd(resp, sizeof(resp),
			     "AT%%XCOEX0=1,1,1565,1586") == 0) {
		LOG_INF("COEX0 -> GNSS L1 LNA enabled");
	}

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("gnss_event_handler_set failed: %d", err);
		test_report("gnss", TEST_FAIL, "evt handler err %d", err);
		return;
	}
	(void)nrf_modem_gnss_fix_interval_set(1);
	(void)nrf_modem_gnss_fix_retry_set(0);

	atomic_set(&pvt_count, 0);
	atomic_set(&agnss_req_pending, 0);
	memset(&latest_pvt, 0, sizeof(latest_pvt));

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("nrf_modem_gnss_start failed: %d", err);
		test_report("gnss", TEST_FAIL, "gnss_start err %d", err);
		return;
	}
	LOG_INF("GNSS started (assisted), tracking for %d s...", duration_seconds);

	uint16_t best_cn0_seen = 0;
	int max_tracked = 0;
	int64_t fix_at_ms = 0;

	for (int t = 1; t <= duration_seconds; t++) {
		k_sleep(K_SECONDS(1));

		/* Service an A-GNSS request raised by the modem (deferred here
		 * from the event handler — the CoAP fetch is blocking). */
		if (atomic_cas(&agnss_req_pending, 1, 0)) {
			agnss_fetch_and_process();
		}

		int tracked, used_in_fix;
		uint16_t max_cn0;

		summarise_pvt(&tracked, &max_cn0, &used_in_fix);
		if (tracked > max_tracked) {
			max_tracked = tracked;
		}
		if (max_cn0 > best_cn0_seen) {
			best_cn0_seen = max_cn0;
		}

		bool fix = (latest_pvt.flags &
			    NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) != 0;

		if (fix && fix_at_ms == 0) {
			fix_at_ms = k_uptime_get();
			LOG_INF("=== FIX acquired at t=%ds (assisted) ===", t);
			log_fix_position();
		}

		if (t % 5 == 0) {
			LOG_INF("  t=%3ds  tracked=%d  in_fix=%d  max_cn0=%u.%u "
				"dB-Hz  fix=%s", t, tracked, used_in_fix,
				max_cn0 / 10, max_cn0 % 10, fix ? "YES" : "no");
			if (tracked > 0) {
				log_satellites();
			}
		}

		/* Once we have a fix, no need to keep the cell + LNA burning;
		 * stop early so the demo moves on. */
		if (fix) {
			break;
		}
	}

	(void)nrf_modem_gnss_stop();
	(void)nrf_cloud_coap_disconnect();
	(void)nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=0");

	bool got_fix = fix_at_ms != 0;

	LOG_INF("--- assisted GNSS complete: fix=%s, max_tracked=%d, "
		"best_cn0=%u.%u dB-Hz ---", got_fix ? "YES" : "no",
		max_tracked, best_cn0_seen / 10, best_cn0_seen % 10);

	if (got_fix) {
		test_report("gnss", TEST_PASS,
			    "assisted fix in %llds, max_SV=%d, best_cn0=%u.%u",
			    fix_at_ms / 1000, max_tracked,
			    best_cn0_seen / 10, best_cn0_seen % 10);
	} else {
		test_report("gnss", TEST_INFO,
			    "no fix; max_SV=%d best_cn0=%u.%u dB-Hz",
			    max_tracked, best_cn0_seen / 10, best_cn0_seen % 10);
	}
}
