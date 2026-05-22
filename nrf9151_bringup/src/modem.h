#ifndef MODEM_H
#define MODEM_H

/* Init nrf_modem_lib and run a few AT commands (AT, %SHORTSWVER, +CGMR).
 * Does not attach to the network.
 */
void modem_probe(void);

/* Blocking lte_lc_connect with live CEREG event logging. Reads signal
 * quality (+CESQ, %XMONITOR) once attached. Returns immediately on error.
 */
void modem_lte_attach(void);

/* Test BOTH radio access technologies in turn (90 s timeout each):
 * LTE-M across all bands, then NB-IoT locked to band 20 (this operator
 * only offers NB-IoT there, and an all-band scan never reaches it in
 * time). Logs attach time + RSRP and files a separate PASS/FAIL
 * test_report for each ("lte_m_attach", "nbiot_attach"). Leaves the
 * modem at CFUN=0 when done. */
void modem_lte_attach_both(void);

/* Put the modem in flight mode (AT+CFUN=4): digital side fully active,
 * RF front-end disabled. Soak for `seconds` and log progress so we can
 * tell whether silent resets are RF-correlated (would NOT happen here)
 * or pure-power (would still happen).
 */
void modem_cfun4_soak(int seconds);

/* Attach attempt with peak-current mitigations (B8 band-lock for NB-IoT
 * Telekom DE + TX power back-off via %XEMPR). Software-only workaround
 * while the supply rail is reworked — won't save a fundamentally
 * underpowered design, but may let attach limp through near threshold.
 */
void modem_lte_attach_lowpower(void);

/* Sweep over a list of LTE bands, attempting an attach on each one in
 * isolation with a per-band timeout. Logs which bands attached, how long
 * each took, and the signal quality on success. Useful for figuring out
 * which bands a SIM/operator is actually willing to give you on a given
 * board+location. Edit the bands[] array at the top of the function in
 * modem.c to change the candidate set.
 */
void modem_band_sweep(void);

#endif
