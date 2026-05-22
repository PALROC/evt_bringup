#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <nrf_modem_at.h>

#include "modem.h"
#include "test_report.h"

LOG_MODULE_REGISTER(modem, LOG_LEVEL_INF);

void modem_probe(void)
{
	char resp[128];
	int err;

	LOG_INF("calling nrf_modem_lib_init()...");
	err = nrf_modem_lib_init();
	LOG_INF("nrf_modem_lib_init() returned %d", err);
	if (err) {
		LOG_ERR("modem init failed; aborting modem probe");
		test_report("modem_init", TEST_FAIL, "lib_init err %d", err);
		return;
	}

	LOG_INF("sending plain AT...");
	err = nrf_modem_at_printf("AT");
	LOG_INF("AT returned %d", err);

	LOG_INF("sending AT%%SHORTSWVER...");
	err = nrf_modem_at_cmd(resp, sizeof(resp), "AT%%SHORTSWVER");
	if (err) {
		LOG_ERR("AT%%SHORTSWVER err=%d", err);
		test_report("modem_init", TEST_FAIL,
			    "%%SHORTSWVER err %d", err);
		return;
	}
	LOG_INF("%s", resp);

	/* Extract a short version snippet to fit the test_report detail. */
	char ver[32] = {0};
	const char *p = strstr(resp, "%SHORTSWVER:");

	if (p) {
		/* skip "%SHORTSWVER:" + spaces */
		p += strlen("%SHORTSWVER:");
		while (*p == ' ') p++;
		size_t i = 0;
		while (*p && *p != '\r' && *p != '\n' && i < sizeof(ver) - 1) {
			ver[i++] = *p++;
		}
	}
	test_report("modem_init", TEST_PASS, "fw=%s",
		    ver[0] ? ver : "ok (parse fail)");

	LOG_INF("sending AT+CGMR (full firmware version)...");
	err = nrf_modem_at_cmd(resp, sizeof(resp), "AT+CGMR");
	if (err) {
		LOG_ERR("AT+CGMR err=%d", err);
	} else {
		LOG_INF("%s", resp);
	}
}

void modem_print_attest_token(void)
{
	char resp[320];
	int err;

	/* AT%ATTESTTOKEN returns the device identity attestation token —
	 * the Base64url string (between quotes) you paste into the nRF Cloud
	 * portal under Security Services -> Claimed Devices -> Claim Device.
	 * Printed to RTT so no UART AT shell is needed on this board.
	 * Requires the modem to be initialised (call after modem_probe()).
	 */
	err = nrf_modem_at_cmd(resp, sizeof(resp), "AT%%ATTESTTOKEN");
	if (err) {
		LOG_ERR("AT%%ATTESTTOKEN failed: %d", err);
		test_report("attest_token", TEST_FAIL, "AT err %d", err);
		return;
	}

	LOG_INF("================ nRF CLOUD CLAIM TOKEN ================");
	LOG_INF("%s", resp);
	LOG_INF("Copy the string between the quotes into nRF Cloud ->");
	LOG_INF("Security Services -> Claimed Devices -> Claim Device,");
	LOG_INF("with an auto-onboarding rule that provisions CoAP certs.");
	LOG_INF("======================================================");
	test_report("attest_token", TEST_INFO, "printed to RTT");
}

static K_SEM_DEFINE(lte_connected_sem, 0, 1);

static const char *reg_str(enum lte_lc_nw_reg_status s)
{
	switch (s) {
	case LTE_LC_NW_REG_NOT_REGISTERED:        return "not registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:       return "registered (home)";
	case LTE_LC_NW_REG_SEARCHING:             return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:   return "denied";
	case LTE_LC_NW_REG_UNKNOWN:               return "unknown";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:    return "registered (roaming)";
	case LTE_LC_NW_REG_UICC_FAIL:             return "UICC fail (SIM?)";
	default:                                  return "?";
	}
}

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("reg status -> %d (%s)", evt->nw_reg_status,
			reg_str(evt->nw_reg_status));
		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			k_sem_give(&lte_connected_sem);
		}
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("cell update tac=0x%x cell_id=0x%x",
			evt->cell.tac, evt->cell.id);
		break;
	default:
		break;
	}
}

void modem_cfun4_soak(int seconds)
{
	char resp[128];
	int err;

	LOG_INF("entering CFUN=4 (flight mode: digital ON, RF OFF)...");
	err = nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=4");
	if (err) {
		LOG_ERR("AT+CFUN=4 failed: %d", err);
		return;
	}
	LOG_INF("CFUN=4 set OK");

	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN?") == 0) {
		LOG_INF("CFUN state: %s", resp);
	}

	LOG_INF("soaking in CFUN=4 for %d s — if NO reset, RF is the trigger;"
		" if reset still happens, supply can't even handle the digital draw",
		seconds);

	for (int t = 0; t < seconds; t += 5) {
		k_msleep(5000);
		LOG_INF("CFUN=4 soak: %d/%d s elapsed (alive)", t + 5, seconds);
	}

	LOG_INF("CFUN=4 soak complete with no reset");
}

void modem_lte_attach_lowpower(void)
{
	char resp[160];
	int err;

	/* %XBANDLOCK and %XEMPR must be set before CFUN=1. After modem init
	 * we're already in CFUN=0, but if a prior step (e.g. cfun4_soak) left
	 * us elsewhere, drop back explicitly so the config commands are
	 * accepted (otherwise +CME ERROR 518 "not allowed in active state").
	 */
	LOG_INF("forcing CFUN=0 before applying band-lock + EMPR config...");
	nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=0");

	/* Runtime (mode 2) bandlock — single band only.
	 *
	 * Supply-rail characterization (step 5.2, see steps.md):
	 *   B8 only  + -1 dB  → STABLE 5+ min (current default)
	 *   B20 only + -1 dB  → brownout
	 *   B8+B20   + -1 dB  → brownout
	 *   any band + 0 dB   → brownout
	 *
	 * B8 is parked as the only known-stable single-band config until the
	 * hardware power supply is reworked. Bit string right-aligned,
	 * LSB = band 1. To experiment again, swap to:
	 *   B20 only:  "10000000000000000000"
	 *   B8 + B20:  "10000000000010000000"
	 */
	LOG_INF("locking to NB-IoT band 20 only (Telekom DE 800 MHz)...");
	err = nrf_modem_at_cmd(resp, sizeof(resp),
			       "AT%%XBANDLOCK=2,\"10000000000000000000\"");
	if (err) {
		LOG_ERR("AT%%XBANDLOCK failed: %d (raw: %s)", err, resp);
	} else {
		LOG_INF("  band lock OK");
	}

	/* %XEMPR: NB-IoT mode = 0, k=0 (apply to all bands), pr=2 (-1.0 dB).
	 * That's the maximum reduction the modem exposes — small, but it's
	 * the only knob.
	 */
	LOG_INF("backing off TX power by 1.0 dB on all NB-IoT bands...");
	err = nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XEMPR=0,0,2");
	if (err) {
		LOG_ERR("AT%%XEMPR failed: %d (raw: %s)", err, resp);
	} else {
		LOG_INF("  TX back-off OK");
	}

	/* Confirm what the modem actually has now. */
	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XBANDLOCK?") == 0) {
		LOG_INF("  %s", resp);
	}
	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XEMPR?") == 0) {
		LOG_INF("  %s", resp);
	}

	lte_lc_register_handler(lte_event_handler);

	LOG_INF("requesting NB-IoT attach (low-power, single band)...");
	int64_t t0 = k_uptime_get();

	err = lte_lc_connect();

	int64_t took_s = (k_uptime_get() - t0) / 1000;

	if (err) {
		LOG_ERR("lte_lc_connect failed after %llds: %d", took_s, err);
		test_report("lte_attach", TEST_FAIL,
			    "connect err %d after %llds", err, took_s);
		return;
	}
	LOG_INF("connected in %llds", took_s);

	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CESQ") == 0) {
		LOG_INF("%s", resp);
	}
	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XMONITOR") == 0) {
		LOG_INF("%s", resp);
	}

	test_report("lte_attach", TEST_PASS,
		    "B20 NB-IoT, attached in %llds", took_s);
}

/* Build a right-aligned LTE bandlock bitstring (LSB = band 1) for a single
 * band. Buffer must hold at least 89 chars (88 band bits + NUL). Out-of-range
 * inputs produce an empty string.
 */
static void band_to_bandlock_string(int band, char *out, size_t bufsize)
{
	const size_t len = 88;

	if (bufsize == 0) {
		return;
	}
	out[0] = '\0';
	if (bufsize < len + 1 || band < 1 || (size_t)band > len) {
		return;
	}
	memset(out, '0', len);
	out[len] = '\0';
	out[len - band] = '1';
}

#define BAND_SWEEP_TIMEOUT_S 90

void modem_band_sweep(void)
{
	/* Bands to probe. NB-IoT in EU is typically B8/B20/B3; LTE-M is
	 * B3/B7/B8/B20. The 91x1 supports up to band 88 — extend this list
	 * for whatever your SIM/operator is likely to accept.
	 */
	static const int bands[] = {1, 3, 8, 20, 28};

	char resp[160];
	char bandstr[89];
	int connected = 0;

	LOG_INF("=== band sweep: %d bands, %d s timeout each ===",
		(int)ARRAY_SIZE(bands), BAND_SWEEP_TIMEOUT_S);

	lte_lc_register_handler(lte_event_handler);

	for (size_t i = 0; i < ARRAY_SIZE(bands); i++) {
		int band = bands[i];
		int err;

		LOG_INF("--- B%d ---", band);

		/* Drop to CFUN=0 so the bandlock change is accepted (the
		 * modem rejects %XBANDLOCK in active state with +CME 518).
		 */
		nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=0");

		/* Build the bandlock string for this band, then format it
		 * straight into nrf_modem_at_cmd. Going through an
		 * intermediate snprintf'd buffer breaks because nrf_modem_at_cmd
		 * is itself printf-style, so the literal '%' in the AT command
		 * name would be reinterpreted as a hex format specifier and
		 * the AT parser sees stack garbage (=> CME error 65536).
		 */
		band_to_bandlock_string(band, bandstr, sizeof(bandstr));
		err = nrf_modem_at_cmd(resp, sizeof(resp),
				       "AT%%XBANDLOCK=2,\"%s\"", bandstr);
		if (err) {
			LOG_ERR("  bandlock B%d failed: %d", band, err);
			continue;
		}

		k_sem_reset(&lte_connected_sem);
		int64_t t0 = k_uptime_get();

		LOG_INF("  starting attach on B%d (timeout %d s)...", band,
			BAND_SWEEP_TIMEOUT_S);
		err = lte_lc_connect_async(NULL);
		if (err && err != -EBUSY) {
			LOG_ERR("  lte_lc_connect_async failed: %d", err);
			continue;
		}

		err = k_sem_take(&lte_connected_sem,
				 K_SECONDS(BAND_SWEEP_TIMEOUT_S));
		int64_t took_s = (k_uptime_get() - t0) / 1000;

		if (err == 0) {
			LOG_INF("  +++ B%d attached in %llds", band, took_s);
			connected++;
			if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CESQ") == 0) {
				LOG_INF("    %s", resp);
			}
			if (nrf_modem_at_cmd(resp, sizeof(resp),
					     "AT%%XMONITOR") == 0) {
				LOG_INF("    %s", resp);
			}
		} else {
			LOG_WRN("  --- B%d timeout after %llds", band, took_s);
		}
	}

	/* Park modem in flight mode at the end so the sweep doesn't leave
	 * the RF up indefinitely.
	 */
	nrf_modem_at_cmd(resp, sizeof(resp), "AT+CFUN=4");

	LOG_INF("=== sweep complete: %d/%d bands attached ===", connected,
		(int)ARRAY_SIZE(bands));
}

void modem_lte_attach(void)
{
	char resp[160];
	int err;

	lte_lc_register_handler(lte_event_handler);

	LOG_INF("requesting NB-IoT attach (lte_lc_connect, blocking)...");
	int64_t t0 = k_uptime_get();

	err = lte_lc_connect();

	int64_t took_s = (k_uptime_get() - t0) / 1000;

	if (err) {
		LOG_ERR("lte_lc_connect failed after %llds: %d", took_s, err);
		return;
	}
	LOG_INF("connected in %llds", took_s);

	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CESQ") == 0) {
		LOG_INF("%s", resp);
	}
	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XMONITOR") == 0) {
		LOG_INF("%s", resp);
	}
}

/* Per-RAT attach timeouts. LTE-M attaches in ~4 s when available, so a
 * short timeout fails fast if it isn't. NB-IoT can take ~80 s here
 * (roaming operator selection cycles through several cells before it
 * registers), so it needs a much longer window + margin. */
#define LTEM_ATTACH_TIMEOUT_S   45
#define NBIOT_ATTACH_TIMEOUT_S  120

/* Attach on a single RAT (LTE-M or NB-IoT) with a timeout. `lock_band`
 * pins the attach to one band (e.g. NB-IoT is only offered on B20 by
 * this operator, and scanning all bands never reaches it inside the
 * timeout); pass 0 to allow all bands. `timeout_s` bounds the attach
 * wait. Reads RSRP on success and files a PASS/FAIL test_report under
 * `report_key`. Leaves the modem at CFUN=0 on exit so the next RAT (or
 * the rest of the demo) starts clean.
 *
 * System mode + band lock can only be changed while the modem is
 * non-active, so we drop to CFUN=0 first; lte_lc_connect_async() then
 * brings it to CFUN=1.
 */
static void attach_one_rat(enum lte_lc_system_mode mode, const char *label,
			   const char *report_key, int lock_band, int timeout_s)
{
	char resp[160];
	int err;

	nrf_modem_at_printf("AT+CFUN=0");

	err = lte_lc_system_mode_set(mode, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("%s: system_mode_set failed: %d", label, err);
		test_report(report_key, TEST_FAIL, "mode_set err %d", err);
		return;
	}

	/* Apply (or clear) the band lock. XBANDLOCK=0 clears any lock left
	 * by a previous RAT/iteration; XBANDLOCK=2,"<bits>" pins one band.
	 * The literal '%' in the AT name must be doubled — nrf_modem_at_cmd
	 * is printf-style. */
	if (lock_band > 0) {
		char bandstr[89];

		band_to_bandlock_string(lock_band, bandstr, sizeof(bandstr));
		err = nrf_modem_at_cmd(resp, sizeof(resp),
				       "AT%%XBANDLOCK=2,\"%s\"", bandstr);
		LOG_INF("--- %s attach (B%d only, %d s timeout) ---", label,
			lock_band, timeout_s);
	} else {
		err = nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XBANDLOCK=0");
		LOG_INF("--- %s attach (all bands, %d s timeout) ---", label,
			timeout_s);
	}
	if (err) {
		LOG_WRN("%s: XBANDLOCK set returned %d (continuing)", label, err);
	}

	k_sem_reset(&lte_connected_sem);
	int64_t t0 = k_uptime_get();

	err = lte_lc_connect_async(NULL);
	if (err && err != -EBUSY) {
		LOG_ERR("%s: connect_async failed: %d", label, err);
		test_report(report_key, TEST_FAIL, "connect err %d", err);
		nrf_modem_at_printf("AT+CFUN=0");
		return;
	}

	err = k_sem_take(&lte_connected_sem, K_SECONDS(timeout_s));
	int64_t took_s = (k_uptime_get() - t0) / 1000;

	if (err) {
		LOG_WRN("%s: attach timeout after %llds", label, took_s);
		test_report(report_key, TEST_FAIL, "timeout after %llds",
			    took_s);
		nrf_modem_at_printf("AT+CFUN=0");
		return;
	}

	/* RSRP from +CESQ field 6 (raw 0..97, dBm = raw - 140; 255 = n/a). */
	int rxlev, ber, rscp, ecn0, rsrq, rsrp_raw;
	int rsrp_dbm = 0;
	int n = nrf_modem_at_scanf("AT+CESQ", "+CESQ: %d,%d,%d,%d,%d,%d",
				   &rxlev, &ber, &rscp, &ecn0, &rsrq, &rsrp_raw);
	if (n == 6 && rsrp_raw != 255) {
		rsrp_dbm = rsrp_raw - 140;
	}

	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XMONITOR") == 0) {
		LOG_INF("%s", resp);
	}

	LOG_INF("%s: attached in %llds, RSRP %d dBm", label, took_s, rsrp_dbm);
	test_report(report_key, TEST_PASS, "%llds, RSRP %d dBm", took_s,
		    rsrp_dbm);

	/* Drop RF so the next RAT starts clean. */
	nrf_modem_at_printf("AT+CFUN=0");
}

/* NB-IoT on this operator/SIM is only offered on band 20 (800 MHz).
 * Scanning all bands never reaches it inside the timeout, so pin it.
 * LTE-M attaches fine across all bands (seen on B3 roaming), so leave
 * it unlocked. */
#define NBIOT_LOCK_BAND 20

void modem_lte_attach_both(void)
{
	lte_lc_register_handler(lte_event_handler);

	attach_one_rat(LTE_LC_SYSTEM_MODE_LTEM,  "LTE-M",  "lte_m_attach",
		       0, LTEM_ATTACH_TIMEOUT_S);
	attach_one_rat(LTE_LC_SYSTEM_MODE_NBIOT, "NB-IoT", "nbiot_attach",
		       NBIOT_LOCK_BAND, NBIOT_ATTACH_TIMEOUT_S);
}
