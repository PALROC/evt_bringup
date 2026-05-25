# A-GNSS bring-up — debugging notes (resume here)

_Last updated: 2026-05-22. Branch: master. Repo: evt_bringup/nrf9151_bringup._

## TL;DR status

The full nRF Cloud chain WORKS end to end:
- Provisioning: device claimed + credentials installed (sec_tag 16842753). ✅
- nRF Cloud CoAP connect + JWT auth: `2.01 Authorized, DTLS CID active`. ✅
- A-GNSS fetch + inject: `injected 3405 bytes of assistance`. ✅

**OPEN ISSUE: GNSS still gets NO FIX.** Tracks only 0-2 SVs at a time,
max CN0 ~25-28 dB-Hz (open sky should be ~40-45). Need 4 SVs
simultaneously for a fix; signal is too weak/intermittent.

The remaining problem is **RF / signal on the GNSS front end**, plus one
**firmware gap** (no location assistance). The cloud plumbing is done.

---

## Device / environment facts

- Device UUID / nRF Cloud ID: `50433842-3232-4058-80b8-291e2dba2ed3`
- IMEI: 355025936304086, Modem FW: mfw_nrf91x1_2.0.4
- nRF Cloud sec_tag (device creds): **16842753** (default)
- Provisioning service CA sec_tag: 42
- SDK: NCS v3.0.2 (Zephyr 4.0.99). NOTE: original NMS bring-up was 3.3.0.
- Cellular SIM: roaming. LTE-M = Movistar ES (B3, ~4s). NB-IoT = Orange
  ES (B20 only, must band-lock B20).
- GNSS antenna LNA powered from nPM1300 LDO1 (always-on, 3.3V, confirmed)
  and gated by COEX0 (AT%XCOEX0=1,1,1565,1586 in gnss.c).

## Build architecture (RAM-driven split)

Wi-Fi (57 KB heap) + the full cloud stack don't fit in 9151/ns RAM. So:

- **Demo build (default, prj.conf)** = Wi-Fi + nRF Cloud CoAP A-GNSS.
  The heavy `nrf_provisioning` lib is NOT included; `provisioning_run()`
  compiles to a cert-check stub (`#else` branch in `src/provisioning.c`).
  ```bash
  cd evt_bringup/nrf9151_bringup
  west build -b nrf9151_nova_evt2/nrf9151/ns -d build_nrf9151_ns_evt2 -p \
    -- -DCONF_FILE=prj.conf \
       -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
  west flash -d build_nrf9151_ns_evt2
  ```

- **One-time provisioning build (overlay-provision.conf)** — only needed
  to provision a NEW board (this device is already provisioned):
  ```bash
  west build -b nrf9151_nova_evt2/nrf9151/ns -d build_provision -p \
    -- -DCONF_FILE=prj.conf \
       -DEXTRA_CONF_FILE=overlay-provision.conf \
       -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
  west flash -d build_provision
  ```
  Press Hall 2, wait for "provisioning complete", reflash the demo build.

Relevant flags in `src/main.c`: `RUN_GNSS_PROBE=1`, `GNSS_ASSISTED=1`,
`RUN_PROVISIONING=1` (stub no-ops once provisioned).

## Demo flow (per Hall-2 press)

modem init → attest-token print → provisioning check (stub: "already
provisioned") → assisted GNSS (gnss_probe_assisted) → LTE-M + NB-IoT
attach → i2c/PMIC/flash → IMU accel → Wi-Fi scan → nRF7000 off →
BLE_START to L15 → loop.

---

## THE OPEN ISSUE: no GNSS fix

Symptom: assistance injects fine, but after `LTE deactivated — GNSS now
has the radio`, GNSS tracks 0-2 SVs at ~25 dB-Hz, never 4 at once, no fix
in 300 s. Every tracked SV shows `el=0 az=0`.

### Two distinct problems

**1. RF / weak signal (the real blocker).** ~25 dB-Hz vs ~40-45 expected
= ~20 dB deficit. Too big for a 50Ω matching mismatch (a few dB); a ~20
dB hit smells like the LNA not actually being enabled/powered. BUT the
earlier UNASSISTED balcony run (CFUN=31, GNSS-only) tracked 12 SVs at the
same COEX0 setting — so the LNA path works there. The assisted run was
likely indoors / poorer sky, AND/OR LTE was starving GNSS (now fixed by
DEACTIVATE_LTE). Must re-test on the balcony with clear sky.

**2. Firmware gap: no location assistance.** `el=0 az=0` on every SV =
the receiver has no approximate position, so it can't compute az/el. Root
cause: `agnss_fetch_and_process()` in `src/gnss.c` does NOT set
`request.net_info` (serving-cell info), so nRF Cloud returns no LOCATION
assist ("cloud cannot provide location assistance data if network info is
NULL"). This speeds a fix but is NOT the no-fix root cause (#1 is).

### Next-session checklist (cheap → expensive)

1. **Open-sky CN0** — flash demo build, run on the BALCONY/outdoors with
   clear sky, read CN0 + `tracked` count AFTER the `LTE deactivated`
   line. This single number decides everything:
   - CN0 ~40+, tracked climbs → path is fine, earlier failures were
     sky/LTE; a fix should land. Then just add location assist (#4).
   - CN0 still ~25 with open sky → real RF deficit, continue:
2. **Multimeter the LNA**: is it drawing current (few mA) and is its
   enable pin actually HIGH when COEX0 is asserted during GNSS? If the
   LNA isn't drawing current → it's off → COEX0 polarity/enable problem,
   not matching.
3. **Flip COEX0 polarity** (free, 1-line): in `src/gnss.c` change the
   `AT%%XCOEX0=1,1,1565,1586` `gnss_pin` arg from `1` to `0`
   (`AT%%XCOEX0=1,0,1565,1586`). If CN0 jumps ~15 dB, that was it.
4. **Add location assistance** (firmware bug fix): populate
   `struct lte_lc_cells_info net_info` with the current serving cell and
   set `request.net_info = &net_info` in `agnss_fetch_and_process()`.
   Mirror the NCS `modem_shell` sample's `serving_cell_info_get()`
   (nrf/samples/cellular/modem_shell/src/gnss/gnss.c). Gives the receiver
   a position seed → computes az/el → faster fix.
5. **GNSS priority mode**: `nrf_modem_gnss_prio_mode_enable()` if LTE
   contention is still a factor.
6. **Only then suspect 50Ω matching** — needs a VNA (return loss at the
   LNA input / antenna feed) or check the matching network components
   against the LNA datasheet reference design.

### PRIME SUSPECT (2026-05-22): LNA VEN voltage-level mismatch

COEX0 is the RIGHT mechanism and is asserted correctly — do NOT switch to
MAGPIO and do NOT invert it. The original NMS bring-up
(`bring-up/9151_nms_bringup/steps.md` step 10, "Reality check") hit the
identical symptom (0 SVs, clear sky) and root-caused it:

> The LNA enable (driven by COEX0) only reaches the 9151's VDDIO_GPIO
> level (= BUCK2). The LNA's VEN min-HIGH threshold = its VCC − 0.3 V,
> where VCC = LDO1. If COEX0_high < LDO1 − 0.3 V, the LNA never enables —
> you hear only the antenna's bare gain (~<25 dB-Hz, occasional SVs, no
> fix), exactly our symptom.

Original board fixed it by pinning LDO1 = the COEX0 swing voltage (both
2.7 V). **The nova EVT2 board sets LDO1 = 3.3 V** assuming COEX0 swings to
BUCK2 = 3.3 V. If the 9151 I/O rail driving COEX0 is actually < 3.0 V
(e.g. 1.8 V VDDIO), the LNA stays off.

CONFIRMED 2026-05-22: user is 100% sure the LNA enable is wired to the
9151 COEX0 pin. LDO1 is set to 3.3 V in the DT. So both are nominally
right — yet still no fix, ~25 dB-Hz.

LNA VEN (enable) has TWO specs vs its VCC (= LDO1):
  - VEN high  >= VCC - 0.3 V   (to turn ON)
  - VEN       <= VCC           (max; exceeding can DAMAGE the part)
=> **LDO1 must EQUAL the measured COEX0 high level.** The nova DT sets
LDO1 = 3.3 ASSUMING COEX0 swings to 3.3; if the 9151 I/O rail is lower
(2.7 / 1.8) the LNA never enables. Original board used 2.7/2.7.

**MEASURE (multimeter, during a GNSS run) — VEN pin at the LNA is the
easiest probe (= the COEX0 net):**
1. LNA VEN voltage when GNSS active — THE DECIDER:
   - ~3.3 V  -> LNA told to turn on; problem is downstream (dead LNA
               from the 4.2 V event? solder? RF matching? antenna).
   - ~2.7/1.8 V -> below VEN threshold (LDO1-0.3=3.0) -> LNA OFF. FIX:
               set LDO1 = that exact voltage (mirror original 2.7/2.7).
   - ~0 V    -> COEX0 not asserting; modem/AT-config path, separate.
2. LDO1 at the LNA VCC pin — confirm 3.3 V actually reaches the LNA.
3. LNA supply current — few mA = on; ~0 = off.

**DO NOT change LDO1 before measuring VEN** — if COEX0 = 3.3 V and we
drop LDO1, VEN > VCC risks damaging the LNA. Set LDO1 = measured COEX0
high, in `palroc_nova/.../nrf9151_nova_evt2_common.dtsi` npm1300_ldo1.

UPDATE 2026-05-22:
- Testing on EVT2, which was NEVER hit by the 4.2 V event (that killed
  the EVT1 IMU). So the LNA is almost certainly GOOD → this is a
  voltage/config issue, fixable in DT, not rework.
- The board DT comment (line ~167) ASSUMES "COEX0 swings to BUCK2
  (3.3 V)". But BUCK2's comment lists its loads as "nRF7000 VDDIO + 3V3
  rail (NOR flash, LSM6DSO)" — NOT the 9151 itself. COEX0 high = the
  9151's OWN I/O supply voltage, which is unverified and may be < 3.0 V.
  THIS is the prime suspect: if the 9151 I/O rail < 3.0 V, COEX0 can't
  cross the LNA VEN threshold (LDO1 3.3 - 0.3 = 3.0). Measure COEX0/VEN;
  if < 3.0, drop LDO1 to match it (e.g. 2.7/2.7 like the original board).

### Questions to answer (HW)

- Which LNA part is on the board? (VEN threshold + min VCC depend on it.)
- Is the GNSS antenna ACTIVE (needs DC bias / bias-T feed) or PASSIVE
  (relies on the external LNA we have)? Determines the correct feed +
  matching topology.
- What rail feeds the 9151 VDDIO_GPIO (sets COEX0 high level)?

---

## Key commits (this session)

- `551b8aa` Stage C: CoAP A-GNSS fetch + inject
- `d93bbb6` split provisioning into overlay (RAM)
- `39992af` agnss buffer 2048→4096 (-105 didn't fit)
- `5d4f278` / `c626d30` skip device-status shadow PATCH on connect (-116)
- `25bc2f5` deactivate LTE after assistance download (GNSS starvation)

## Kconfig gotchas already solved (don't re-hit these)

- `NRF_CLOUD_AGNSS` depends on `MODEM_INFO_ADD_NETWORK=y`.
- `NRF_CLOUD_COAP` selects `LTE_PROPRIETARY_PSM_REQ` → needs
  `LTE_LC_PSM_MODULE=y`.
- `NRF_PROVISIONING_SCHEDULED` doesn't exist in 3.0.2 (use AUTO_INIT=n +
  manual trigger).
- `nrf_provisioning` needs `CONFIG_REBOOT=y` (internal sys_reboot).
- `NRF_CLOUD_SEND_SHADOW_INFO` is auto-derived (not settable); disable
  `NRF_CLOUD_SEND_DEVICE_STATUS_CONN_INF=n` instead.
- `agnss_buf` must be 4096 (filtered response still > 2048).
- net_mgmt event handler `mgmt_event` is uint32_t in Zephyr <4.1
  (3.0.2), uint64_t in >=4.1 — see wifi_probe.c version guard.

---

## SPI3 flash on EVT2 (2026-05-22)

Symptom: JEDEC reads garbage, not `EF 40 18`. With L15 held in reset
(HOLD_L15_IN_RESET=1, no bus contention) + correct pinctrl: `17 17 17`.

- **Pinctrl is CORRECT**: SCK=P0.13, MOSI=P0.15, MISO=P0.14 — confirmed
  against the original palroc board pinctrl that read `EF 40 18`. (NOTE:
  steps.md step 12 PROSE reverses MOSI/MISO — the pinctrl file is the
  source of truth. A test-swap to MOSI=P0.14/MISO=P0.15 made it WORSE
  (`00 00 00`), confirming P0.15/P0.14 is right. Reverted.)
- This matches the original board#1 broken-flash fingerprint (`17 17 17`,
  IMU WHO_AM_I=0x17) documented in bring-up/9151_nms_bringup/steps.md
  step 12. Voltage + SPI mode already exonerated there.

**Prime suspect (cheap, do first): `/WP` (pin 3) + `/HOLD` (pin 7) on the
W25Q128JV are FLOATING on this EVT2 board** — user hasn't soldered the
3V3 bridge yet. Floating `/HOLD` low halts the flash → no DO → garbage
regardless of MOSI/MISO. FIX: solder pins 3 & 7 to 3V3.

**If still failing after the bridge:** step 12.6.5 — the L15 `spi00`
(status=okay, cs-gpios P2.05/P1.14) drives the shared MISO every normal
boot, contending with the flash; sustained MISO contention can
ELECTRICALLY DAMAGE the flash output driver. Holding L15 in reset stops
new contention but won't revive a damaged chip → scope SCK/MOSI/MISO +
reflow, or replace the flash.

**Design fix (product, not just test):** L15 `spi00` must NOT drive the
shared bus unless it's the active master. Currently it contends on every
boot. Disable or park `spi00` high-Z on the L15 unless it owns the bus
(mirror the spi21/Wi-Fi "enabled, no cs-gpios, high-Z" approach), or
arbitrate via the UART link.

### CONFIRMED ROOT CAUSE (2026-05-22): /WP + /HOLD have no pull-ups

User identified from the schematic: the W25Q128JV /WP (pin 3) and /HOLD
(pin 7) are MISSING their 100 kΩ pull-ups on EVT2, so they float. In
standard SPI mode both must be held HIGH; a floating /HOLD sits low ->
flash output goes high-Z and it ignores SPI -> no DO -> the `17 17 17`
"flash not responding" we see (with correct pinctrl + L15 isolated).

These pins are NOT wired to any MCU GPIO -> FIRMWARE CANNOT FIX THIS.
Hardware only:
- Quick: solder-bridge pin 3 and pin 7 to pin 8 (VCC/3V3).
- Proper: 100 kΩ pull-up from each of pin 3 and pin 7 to 3V3.
- Next board spin: add both 100 kΩ pull-ups to the schematic.

After the bridge/pull-ups, JEDEC should read EF 40 18. If it STILL fails
only then revisit contention/damage (L15 spi00 on shared MISO) or solder
joints. Also note: P0.19 -> L15 nRESET has NO hardware pull-up either
(steps.md step 6.4) — HOLD_L15_IN_RESET drives it low so reset works, but
the L15's own nRESET floats by default.

### /CS pull-up also likely missing (2026-05-22)

User checked other W25Q128 designs: /CS (pin 1) normally has a pull-up;
this board likely lacks it too. Role: keeps /CS HIGH (deselected) when
the host isn't driving it (MCU boot/reset, shared-CS-net idle between the
two MCUs) — without it /CS can float low, flash spuriously selected /
latched into a bad state.

** /CS REWORK IS DIFFERENT from /WP and /HOLD: **
- /CS (pin 1): 10-100 kOhm PULL-UP to 3V3 ONLY. Do NOT hard-bridge to
  VCC — the MCU must pull /CS low to select the flash; a hard tie keeps
  it permanently deselected (never responds).
- /WP (pin 3), /HOLD (pin 7): pull-up OR hard bridge to 3V3 (always-high
  in basic SPI).

Full flash rework: /CS pull-up + /WP pull-up/bridge + /HOLD pull-up/
bridge, all to 3V3. /HOLD is the most likely to unblock the read; /CS +
/WP are correctness/robustness. Add all three to the next board spin.
