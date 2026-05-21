#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>

#include "gnss.h"
#include "test_report.h"

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

/* Latest PVT frame published by the modem GNSS event callback.
 * Read by the periodic-status loop in gnss_probe(). The struct contains
 * a satellite array sv[] and a flags byte indicating fix validity.
 */
static struct nrf_modem_gnss_pvt_data_frame latest_pvt;
static atomic_t pvt_count = ATOMIC_INIT(0);

static void gnss_event_handler(int event)
{
	if (event != NRF_MODEM_GNSS_EVT_PVT) {
		return;
	}
	if (nrf_modem_gnss_read(&latest_pvt, sizeof(latest_pvt),
				NRF_MODEM_GNSS_DATA_PVT) == 0) {
		atomic_inc(&pvt_count);
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
