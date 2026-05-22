#ifndef GNSS_H
#define GNSS_H

/* Run a GNSS bring-up test for `duration_seconds`. Configures COEX0 to
 * enable the external LNA (palroc board's GNSS antenna LNA is powered
 * from nPM1300 LDO1 and gated by COEX0), switches the modem to
 * GNSS-only mode (CFUN=31), starts continuous tracking, and logs every
 * 5 seconds: number of SVs being tracked, best CN0, and fix state.
 * Stops GNSS at the end and parks the modem in CFUN=0.
 *
 * Requires the modem already initialised via nrf_modem_lib_init()
 * (i.e. modem_probe() must have been called first).
 */
void gnss_probe(int duration_seconds);

/* Assisted GNSS (A-GNSS). Like gnss_probe() but fetches assistance data
 * (ephemerides/almanac/time/location) from nRF Cloud over CoAP and
 * injects it into the modem for a fast first fix even on weak/blocked
 * sky. Uses LTE-M + GPS coexistence (CFUN=1), so it attaches LTE and
 * connects to nRF Cloud first — requires the device to be provisioned
 * (see provisioning_run()). Stops early once a fix is acquired. Parks
 * the modem at CFUN=0 on exit.
 */
void gnss_probe_assisted(int duration_seconds);

#endif
