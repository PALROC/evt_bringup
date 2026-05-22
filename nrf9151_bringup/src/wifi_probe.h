#ifndef WIFI_PROBE_H
#define WIFI_PROBE_H

/* Trigger a single passive Wi-Fi scan on the nRF7000 and log every AP
 * found (SSID, BSSID, channel, RSSI, security mode, MFP). Blocks until
 * the scan completes (driver fires NET_EVENT_WIFI_SCAN_DONE) or the
 * timeout expires. Passive-only is enforced because nPM1300 BUCKs are
 * limited to ~250 mA and active scan exceeds that — see project memory
 * project_palroc_nrf7000_supply.md.
 *
 * Note: scan results depend on the antenna being routed to the Wi-Fi
 * RX path through the BGS12SN6E6 RF switch. If SW_CTRL0 isn't asserting
 * the RF switch into Wi-Fi mode, this will return zero APs even if the
 * Wi-Fi chip itself is healthy.
 */
void wifi_passive_scan(int timeout_seconds);

/* Drive the nRF7000's BUCK_EN (P0.28) and IOVDD_CTL (P0.29) LOW so the
 * load-switches open and the chip is unpowered. Idempotent and ISR-safe.
 * Also called automatically from the fatal-error handler so a runaway
 * firmware condition cannot leave the Wi-Fi chip burning power while the
 * SoC is dead.
 */
void wifi_emergency_off(void);

#endif
