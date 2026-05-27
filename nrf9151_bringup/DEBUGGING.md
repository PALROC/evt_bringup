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

---

## RESOLVED 2026-05-25: it was the antenna, not the board

**Root cause: passive-antenna front-end + active antenna used for testing.**

EVT2's GNSS section is copied from the Thingy:91 X — a passive-antenna
topology: antenna -> on-board LNA (gated by COEX0, powered from LDO1)
-> 9151 GNSS input. NO bias-T on the antenna feed.

We had been testing with an active GPS antenna (the kind with built-in
LNA in the antenna head needing DC bias up the coax). On EVT2 that bias
is not provided -> the antenna's internal LNA stays unpowered -> the
antenna head sits as a dead/absorbing element -> no signal reaches the
on-board LNA -> 0 SVs.

DK appeared to "work with the same antenna" only because the DK board
biases its U.FL net (it supports active antennas by design). Same
antenna, two different board behaviors -> the asymmetry was the
antenna-type mismatch, NOT a bug on EVT2.

**Firmware-side fixes that DID make the board correct (keep these):**
- `%XCOEX0=1,1,1565,1586` must be sent BEFORE any modem activity
  (NRF_MODEM_LIB_ON_INIT hook via `modem_antenna` lib). Setting it
  late = COEX0 stays at 0 V = LNA never enables.
- Auto-enable MODEM_ANTENNA on the nova board via Kconfig.defconfig
  (`default y if NRF_MODEM_LIB`), matching the DK boards.

**Hardware-side note for EVT3:**
- For passive antennas only: current topology is correct, no changes.
- For active-antenna flexibility on the same board: add a small bias-T
  on the antenna feed (1 µH series from a 3 V rail to the antenna
  trace + DC-block cap before LNA input).

**Action: buy a passive GPS patch antenna with U.FL connector
(~$3-5, "passive 1575 MHz patch antenna U.FL").** Plug into EVT2 ->
expect CN0 30-45 outdoors + cold-start fix in 1-3 min.

**Diagnostic that closed it:** COEX0 multimeter reading transitioned
from 0 V / 0.8 V / 1.6 V (duty-cycled during acquisition) to a steady
3.3 V (sustained tracking after lock) — confirming the LNA enable path
works, and the issue is upstream of the on-board LNA = the antenna.

---

## MAJOR PIVOT 2026-05-26: the EVT2 flash chip is ALIVE — 9151-side problem, NOT chip damage

**TL;DR — the entire "contention-damage killed the flash" theory is FALSIFIED.**
The EVT2 flash is a genuine, healthy Winbond W25Q128JV and reports its
JEDEC ID perfectly when read from the nRF54L15 side of the shared SPI3
bus. Every garbage read we got from the 9151 side today (and historically)
is a 9151-side issue, not chip damage.

### The decisive experiment

Built a sibling test app `evt_bringup/flash_test_l15_evt2/` that drives
the L15's own spi00 master onto the *same* shared SPI3 nets and reads
JEDEC at CS=P2.5. Pre-conditions: 9151 erased (so its GPIOs are high-Z
inputs and don't drive the bus), TF-M peripherals released to NS.

Result, after multiple iterations, polling every 5 s:

```
iter 3: JEDEC ef 40 18   PASS  Winbond W25Q128 (128 Mbit) - original EVT2 part
iter 4: JEDEC ef 40 18   PASS  Winbond W25Q128 (128 Mbit) - original EVT2 part
```

Clean, repeatable, valid JEDEC. **The chip is fully functional.**

### What was actually broken (and rules out what wasn't)

| Component | Status now |
|---|---|
| EVT2 flash chip | ✅ Healthy. Genuine Winbond. |
| L15 -> chip SPI3 path | ✅ Works perfectly. |
| L15 SPIM00 peripheral | ✅ Works. |
| TF-M release on /ns | ✅ Works with right configs (see below). |
| Shared SPI3 bus architecture | ✅ Electrically sound. |
| **9151 -> chip SPI3 read** | ❌ **Still broken. Different problem than we thought.** |

### What this falsifies from earlier in this debug doc

- **"L15 contention damages the flash chip" (steps.md 12.6.5 / sections above)**
  — false. The chip is alive on a board where it was supposedly damaged.
- **"EVT1 chip is damaged - EE 40 00 = output stage degraded"** — likely
  also false. Probably the same 9151-side issue (or a clip-test artifact).
  Need to confirm by running flash_test_l15 on EVT1 too.
- **"Replace the flash chip" as the EVT3 hardware action** — not necessary.
  The chips work. EVT3 just needs the 9151-side issue understood.

### TF-M / SPIM00 gotchas that took the longest to figure out

1. **SPIM00 minimum frequency = 1.016 MHz** (= 128 MHz / 126, the max
   prescaler divisor). Setting `spi_config.frequency = 1000000` or
   anything lower returns `NRFX_ERROR_INVALID_PARAM` (0x0bad0004) at
   nrfx_spim_init. SPIM00 is the L15's FAST instance with a fixed
   128 MHz core. Set freq to ~4 MHz to be safely in range.
   - Source: nRF54L15 product spec spim.html + DevZone Q&A 126079.
2. **TF-M owns SPIM00 + GPIO2 by default on /ns.** Release them to NS
   with these in prj.conf (same pattern as the board's existing
   `CONFIG_NRF_UARTE30_SECURE=n` for uart30):
   ```
   CONFIG_NRF_SPIM00_SECURE=n
   CONFIG_NRF_GPIO2_SECURE=n
   CONFIG_NRF_GPIO1_SECURE=n    # only if you use the 2nd cs-gpios entry
   CONFIG_NRF_OSCILLATORS_SECURE=n   # safe to release even if unused
   ```
3. Without the TF-M release: first SPIM register access from NS bus-
   faults with SECURE FAULT at addr 0x4 (plus the same nrfx error).
4. With TF-M released but freq too low: no fault, but nrfx still returns
   0x0bad0004. Two distinct failure modes that look superficially similar.

### What we now actually need to investigate (next session)

The 9151-side SPI3 read still produces shifted/zero/garbage bytes from the
*same chip* the L15 reads cleanly. Hypotheses, cheap to expensive:

1. **9151 MISO trace damage** (most likely) — the 9151's pin P0.14 or
   its short trace to the shared bus may be electrically degraded.
   Suggests: scope MISO at the 9151 pin AND at the chip DO pad
   simultaneously while running the bring-up SPI read. If the chip's
   signal is clean at its pad but garbled at the 9151's pin, MISO trace
   is the suspect.
2. **9151 SPI3 peripheral / pad damage** from some prior event (we
   thought the 4.2 V event was confined to EVT1, but might have touched
   EVT2 GPIOs too).
3. **9151-side SPI3 driver timing margins** too tight on EVT2's layout
   (less likely — same Zephyr driver, same SoC family, worked on DK).
4. **The L15 reading the chip in isolation hides a contention issue
   the 9151 hits when the L15 is also booted.** Test: erase the L15
   too, then re-run the 9151's flash JEDEC. If still broken, 9151 side
   is genuinely faulty independent of bus contention.

### Action items

- [ ] Run flash_test_l15_evt2 with the 9151 NOT erased — does the L15
      still read cleanly while the 9151 is driving the bus? Tells us if
      the L15 SPIM00 is robust to contention.
- [ ] Confirm the EVT1 chip is also alive by running the same L15-side
      probe on EVT1 (need to port flash_test_l15_evt2 to the EVT1 L15
      board target). If it reads EF 40 18, the "chip damage" story is
      fully dead and we have one unified 9151-side problem to chase.
- [ ] Scope MISO at 9151 pin vs chip DO pad on EVT2 with the 9151 SPI
      flash probe running. This is the diagnostic that probably names
      the 9151-side failure mode.
- [ ] Update the 9151-side bring-up `spi_probe.c` to accept the actual
      W25Q128JV JEDEC `EF 40 18` (already correct) AND consider what to
      do if the 9151-side never reads it (the chip works fine via L15).
- [ ] EVT3 design: the underlying architecture (shared SPI3, two
      masters) appears electrically OK after all. Still wise to add an
      arbitration mechanism or bus switch, but it's no longer the
      smoking gun we thought it was.

### Files added today (keep these — they're decisive A/B test scaffolding)

- `evt_bringup/flash_test_dk/`        — DK-side JEDEC probe (validates chip on a known-good board)
- `evt_bringup/flash_test_evt1/`      — EVT1-side JEDEC probe via 9151
- `evt_bringup/flash_test_l15_evt2/`  — **THE WINNING APP**: EVT2-side JEDEC probe via L15

---

## RESOLVED 2026-05-26 late night: the bug is in the Zephyr SPI driver, NOT the silicon

**Headline:** SPIM3 on EVT2's 9151 works perfectly when driven by raw
register writes through Nordic's `nrf_spim_*` HAL. The exact same
peripheral, same chip, same pins, same 1 MHz clock, returns `00 00 00`
when the Zephyr `spi_transceive()` API drives it. So the bug is in the
Zephyr SPI driver glue (`spi_nrfx_spim.c` or `nrfx_spim`), not in the
silicon or the hardware.

### The decisive evidence

`evt_bringup/flash_test_evt2_raw_spim/` boot sequence on EVT2:

```
1) bit-bang baseline (proves chip alive)         -> ef 40 18  PASS
2) raw SPIM3 via HAL (configures registers, fires START):
   pre-transfer registers:   ENABLE=7, PSEL.SCK/MOSI/MISO connected,
                             FREQUENCY=0x10000000 (1M), CONFIG=0,
                             TXD/RXD ptrs valid, AMOUNT=0
   wait for EVENTS_END...    fires after 14 polls / 0 ms
   post-transfer registers:  TXD.AMOUNT=4, RXD.AMOUNT=4,
                             EVENTS_END=1, EVENTS_STARTED=1,
                             EVENTS_ENDTX=1, EVENTS_ENDRX=1
   rx_buf raw:               00 ef 40 18
   ==>  RAW SPIM3 JEDEC = ef 40 18  PASS — chip alive via raw SPIM3
3) raw SPIM3 READ-DATA at addr 0x000000 (looking for the L15's PALR):
   READ raw bytes:
     cmd/addr echo:  00 00 00 00
     data (8 bytes): 50 41 4c 52  07 00 00 00
   ==>  *** PALR FOUND via raw SPIM3 *** counter=7
```

So the full read-side data path is proven working at the silicon /
DMA / pin / bus level. The L15 wrote, the 9151 reads it back via raw
HAL.

### Production fix path (the actual answer for EVT2)

`evt_bringup/nrf9151_bringup/src/spi_probe.c` currently uses
`spi_transceive()`. **Replace it with the raw HAL pattern** from
`flash_test_evt2_raw_spim/src/main.c`. The key bits:

```c
#include <hal/nrf_spim.h>

static uint8_t tx_buf[N] __aligned(4);
static uint8_t rx_buf[N] __aligned(4);

/* one-time at init */
NRF_SPIM_Type *spim = NRF_SPIM3;
spim->ENABLE = 0;
spim->PSEL.SCK  = 13; spim->PSEL.MOSI = 15; spim->PSEL.MISO = 14;
/* nRF91 SPIM has NO PSEL.CSN — drive CS via gpio_pin_set */
spim->FREQUENCY = NRF_SPIM_FREQ_1M;
spim->CONFIG    = 0;  /* mode 0, MSB-first */
spim->ENABLE    = 7;

/* per transfer */
spim->TXD.PTR = (uint32_t)tx_buf;
spim->TXD.MAXCNT = len;
spim->RXD.PTR = (uint32_t)rx_buf;
spim->RXD.MAXCNT = len;
spim->EVENTS_END = 0;
gpio_pin_set(gpio0, CS_PIN, 0);
spim->TASKS_START = 1;
while (spim->EVENTS_END == 0) { /* < 100 us; bound with a uptime check */ }
gpio_pin_set(gpio0, CS_PIN, 1);
```

That's the unblocker — no Nordic engineering needed, no hardware
rework, just don't go through `spi_transceive()` for SPIM3 on this
specific path.

### What's still open

- The exact bug inside `spi_nrfx_spim.c` or `nrfx_spim` — likely in
  the buffer-copy-to-RAM path, CS-via-cs-gpios timing, or some other
  NCS 3.0.2-specific glue. Worth a precise DevZone bug report. We
  have a minimal reproducer (flash_test_evt2_minimal fails) paired
  with a minimal workaround (flash_test_evt2_raw_spim works) — two
  apps differing only in the software layer.
- Whether this affects current NCS too is unknown; we're locked to
  3.0.2 for now.

### Commits

- `4841a1d` — DK pair reproducer + all initial test apps
- `b6054ab` — flash_test_evt2_raw_spim + the silicon-works finding
- `6d02ec6` — raw SPIM3 PALR read (arbitrary-address proof)

