# nRF9151 NMS bring-up — step log

Running log of peripherals brought up on the `palroc/nrf9151dk` board.
Each step lists what was added, the relevant file changes, and the
verification result.

---

## Step 1 — nPM1300 I²C presence check

**Goal:** Confirm the nPM1300 PMIC is reachable on the board's I²C bus.

**Hardware:** nPM1300 wired to the nRF9151 on i2c2:
- SCL → P0.08
- SDA → P0.09

**Changes:**
- `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common-pinctrl.dtsi`
  added `i2c2_default` / `i2c2_sleep` pinctrl groups (TWIM SCL=P0.08,
  SDA=P0.09, internal `bias-pull-up`).
- `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common.dtsi`
  enabled `&i2c2` at `I2C_BITRATE_STANDARD` (100 kHz) with the new
  pinctrl. No I²C children yet — the bus is exposed via
  `DT_NODELABEL(i2c2)` for the app.
- `prj.conf` — added `CONFIG_I2C=y`.
- `src/main.c` — full bus scan (0x03..0x77) using zero-byte writes,
  followed by a periodic ACK check at 0x6b every 5 s.

**Result (RTT):**
```
[00:00:00.402,709] <inf> main: nRF9151 bring-up: i2c2 probe (SCL=P0.08, SDA=P0.09)
[00:00:00.402,740] <inf> main: I2C scan on i2c@a000:
[00:00:00.404,418] <inf> main:   device found at 0x0b
[00:00:00.423,187] <inf> main:   device found at 0x6b
[00:00:00.425,750] <inf> main: nPM1300 ACK at 0x6b
[00:00:05.426,055] <inf> main: nPM1300 ACK at 0x6b
```

**Verdict:** PASS. nPM1300 ACKs at its expected address 0x6b.

**Open question:** Extra ACK at 0x0b. nPM1300 only owns 0x6b, so this
is either a zero-byte-probe phantom on the nRF TWIM, or another chip
on the bus. To confirm, re-scan with `i2c_read` of 1 byte instead of
`i2c_write` of 0 bytes — phantoms drop, real devices stay.

---

## Step 2 — SPI3 bring-up: W25Q128JV flash + LSM6DSO IMU

**Goal:** Confirm flash and IMU respond on the SPI3 bus.

**Hardware:** SPI3 wired as:
- SCK  → P0.13
- MISO → P0.14
- MOSI → P0.15
- Flash CS (W25Q128JVPIQTR) → P0.12
- IMU   CS (LSM6DSO)         → P0.16

**Changes:**
- `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common-pinctrl.dtsi`
  added `spi3_default` / `spi3_sleep` pinctrl groups for SCK/MOSI/MISO.
- `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common.dtsi`
  enabled `&spi3` (SPIM, mode 0). Also `&gpio0` and `&gpiote` —
  both ship `status = "disabled"` in nRF91's peripherals dtsi, and
  software-managed CS needs the GPIO controller instantiated.
- `prj.conf` — added `CONFIG_SPI=y` and `CONFIG_GPIO=y`.
- `src/main.c` — flash JEDEC ID read (`0x9F` + 3 bytes), checks against
  `EF 40 18`. IMU `WHO_AM_I` read (reg `0x0F` with bit 7 set), checks
  against `0x6C`. Both via raw `spi_transceive` with software CS
  (`spi_cs_control` + `gpio_dt_spec` constructed in C).

**Result (RTT):**
```
[00:00:00.403,717] <inf> main: nRF9151 bring-up: i2c2 + spi3 probe
[00:00:00.403,747] <inf> main: I2C scan on i2c@a000:
[00:00:00.424,163] <inf> main:   device found at 0x6b
[00:00:00.426,513] <inf> main: --- SPI3 flash @ CS=P0.12 ---
[00:00:00.426,635] <inf> main: flash JEDEC: ef 40 18 (expected EF 40 18 for W25Q128JV)
[00:00:00.426,635] <inf> main:   -> W25Q128JV detected: PASS
[00:00:00.426,635] <inf> main: --- SPI3 IMU @ CS=P0.16 ---
[00:00:00.426,727] <inf> main: imu WHO_AM_I (0x0F) = 0x00 (expected 0x6C for LSM6DSO)
[00:00:00.426,757] <wrn> main:   -> unexpected WHO_AM_I: FAIL
[00:00:00.426,757] <inf> main: bring-up probes complete
```

**Verdict:**
- W25Q128JV flash: **PASS** (correct JEDEC ID).
- LSM6DSO IMU: **FAIL** (WHO_AM_I = 0x00).
- nPM1300 PMIC re-confirmed at 0x6b. The 0x0b ghost from step 1 did
  not reappear this run — leaning toward "phantom from zero-byte probe"
  rather than "actual nRF54L15 ACK". Still worth re-checking with the
  nRF54L15 powered and a 1-byte read-based scan.

**Triage notes for the IMU FAIL:**
The flash works on the same SCK/MOSI/MISO at SPI mode 0, so the bus
electrically is fine. The 0x00 reply with no response is consistent
with the LSM6DSO never exiting its default I²C mode — which it does on
the first falling edge of CS. Likely causes, in order of probability:

1. **P0.16 not reaching the IMU CS pad** (trace/net mismatch). LSM6DSO
   stays in I²C mode → never drives MISO → bus floats → reads 0x00.
2. **IMU not powered** — verify VDD = 1.7–3.6 V at the chip.
3. **SPI mode mismatch** — try mode 3 (`CPOL=1, CPHA=1`).
   Flash works on mode 0, so this is unlikely but quick to test.

Next sub-step: scope P0.16 during a `spi_imu_whoami()` loop and confirm
it idles high and pulses low.

---

## Step 3 — User LEDs on GPIO0

**Goal:** Confirm the three on-board user LEDs can be driven from the
nRF9151.

**Hardware (per user):**
- LED blue  → P0.04
- LED green → P0.05
- LED red   → P0.10
- All three driven through MOSFET stages.

**Changes:**
- `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common.dtsi`
  added a `leds { compatible = "gpio-leds"; ... }` block with three
  child nodes (`led_blue`, `led_green`, `led_red`) and matching
  `aliases` entries (`led-blue`, `led-green`, `led-red`).
- `src/main.c` — `blink_one()` helper drives each LED for 1 s on / 1 s
  off, three cycles per colour. `leds_walk()` runs blue → green → red.
- `prj.conf` — `CONFIG_GPIO=y` (already set in step 2).
- Boot now waits 500 ms before any probe and 100 ms between phases,
  to give the W25Q128JV its tPUW after PMIC rails settle.

**Polarity tuning:**
First attempt used `GPIO_ACTIVE_LOW` on the assumption of a P-MOSFET
high-side switch. Observed behaviour was the **red LED toggling 3×
then ending stuck ON** — which only fits hardware that is active-HIGH
(the loop's last `set_dt(led, 0)` = "inactive" was driving the pin
HIGH, lighting the LED). Reverted all three to `GPIO_ACTIVE_HIGH`
(N-MOSFET low-side or equivalent: GPIO high → MOSFET on → LED on).

**Result:**
- LED red (P0.10): **PASS** — blinks 1 s on / 1 s off × 3 and ends OFF.
- LED blue (P0.04): **FAIL** — no visible activity.
- LED green (P0.05): **FAIL** — no visible activity.

**Verdict:** Partial PASS. Software LED driver is correct (proven by
red); blue/green are a hardware/schematic question.

**Open question:** Why don't blue and green light up?
- Are the LEDs actually populated on the board?
- Do the nets `LED_BLUE` / `LED_GREEN` actually terminate at P0.04 /
  P0.05 in the schematic, or are those reserved for something else?
- Does each colour use the same MOSFET polarity, or could blue/green
  use a different driver topology than red?
- A scope on P0.04 and P0.05 during `leds_walk()` would settle whether
  the SoC is toggling them at all (Zephyr side healthy) and isolate
  the failure to the analog stage.

---

## Step 4 — Cellular modem alive (no LTE attach yet)

**Goal:** Confirm the nRF9151 modem core boots and accepts AT commands.
No LTE registration or DNS yet — those are blocked until the modem
subsystem itself is proven.

**Pre-step (out of repo):** flashed cellular modem firmware
`mfw_nrf91x1_2.0.4.zip` over SWD using the nRF Connect for Desktop
Programmer. The chip ships from Nordic with PTI (Production Test
Image) only, which is **not** a working cellular firmware — any
`nrf_modem_lib_init()` against PTI ends in
`<err> nrf_modem: Modem has crashed, reason 0xfff, PC: 0x0`.

**Changes:**
- `prj.conf` — added `CONFIG_NRF_MODEM_LIB=y`.
- `src/main.c` — `modem_probe()`: `nrf_modem_lib_init()` then three
  AT commands directly via `nrf_modem_at_cmd()` / `nrf_modem_at_printf()`:
  - `AT` — sanity check
  - `AT%SHORTSWVER` — short modem firmware version
  - `AT+CGMR` — full modem firmware version
  No `lte_lc`, no DNS, no netif setup — purest possible probe.

**Result (RTT):**
```
[00:00:00.886,322] <inf> main: --- Modem (init + AT only, no LTE attach) ---
[00:00:00.886,322] <inf> main: modem: calling nrf_modem_lib_init()...
[00:00:01.135,284] <inf> main: modem: nrf_modem_lib_init() returned 0
[00:00:01.135,284] <inf> main: modem: sending plain AT...
[00:00:01.135,620] <inf> main: modem: AT returned 0
[00:00:01.135,620] <inf> main: modem: sending AT%SHORTSWVER...
[00:00:01.136,199] <inf> main: modem: %SHORTSWVER: nrf91x1_2.0.4
OK
[00:00:01.136,199] <inf> main: modem: sending AT+CGMR (full firmware version)...
[00:00:01.136,657] <inf> main: modem: mfw_nrf91x1_2.0.4
OK
```

**Verdict:** PASS. Modem subsystem boots cleanly, AT interface
responsive, firmware is `mfw_nrf91x1_2.0.4`.

**Implication for the previous crash:** the original
`reason 0xfff, PC: 0x0` was 100% the missing modem firmware, not power
or board layout. No bulk-cap rework needed for this failure mode.
(Power-rail headroom is still relevant for LTE TX bursts later — keep
it on the radar for step 5.)

**Next sub-step (Step 5 — LTE attach):**
1. Confirm antenna is connected to the LTE port.
2. Confirm an active SIM is inserted, carrier known.
3. Pick LTE-M vs NB-IoT (`CONFIG_LTE_NETWORK_MODE_LTE_M` or
   `..._NBIOT`).
4. Re-enable `lte_lc_connect()` and watch for attach + RSRP/RSRQ.

---

## Step 5 — LTE attach (NB-IoT, Telekom DE) — BLOCKED on power

**Goal:** Bring up the cellular link (attach to the NB-IoT network).

**Hardware state going in:**
- LTE antenna connected.
- GNSS antenna connected.
- Telekom DE SIM (proven good on the Nordic nRF9151 DK).
- Modem firmware `mfw_nrf91x1_2.0.4` confirmed by step 4.

**Code refactor done in this step:** main.c was getting unwieldy, so
the bring-up code was split into per-subsystem modules. Each module
has its own `LOG_MODULE_REGISTER` so RTT lines now show
`<inf> modem: ...`, `<inf> spi_probe: ...`, etc.
- `src/main.c` — orchestration only.
- `src/leds.{c,h}` — `boot_indicator()`, `leds_walk()`.
- `src/i2c_probe.{c,h}` — `i2c_scan()`.
- `src/spi_probe.{c,h}` — `spi_flash_jedec()`, `spi_imu_whoami()`.
- `src/modem.{c,h}` — `modem_probe()`, `modem_lte_attach()`.
- `src/diag.{c,h}` — `diag_print_reset_reason()` + heartbeat thread.
- `CMakeLists.txt` — registers all six .c files.

**Other changes in this step:**
- `prj.conf` — `CONFIG_LTE_LINK_CONTROL=y`,
  `CONFIG_LTE_NETWORK_MODE_NBIOT=y`, `CONFIG_HEAP_MEM_POOL_SIZE=2048`,
  `CONFIG_MAIN_STACK_SIZE=4096`. Diagnostics:
  `CONFIG_HWINFO=y`,
  `CONFIG_LTE_LINK_CONTROL_LOG_LEVEL_INF=y`,
  `CONFIG_NRF_MODEM_LIB_LOG_LEVEL_INF=y`,
  `CONFIG_LOG_BUFFER_SIZE=4096`.
- `modem.c` — `modem_lte_attach()` registers an `lte_lc` event handler
  that logs CEREG transitions, then calls `lte_lc_connect()` blocking,
  then reads `AT+CESQ` and `AT%XMONITOR` once attached.
- `diag.c` — prints reset cause via `printk` (immediate, bypasses the
  deferred LOG queue) AND via LOG (decoded). Calls
  `hwinfo_clear_reset_cause()` so the next reset's cause is fresh.
  Heartbeat thread (`K_PRIO_PREEMPT(7)`) logs every 2 s as proof of
  life while main is blocked.
- `main.c` — `boot_indicator()` (red LED 1 s pulse) + a 5 s
  `POST_RESET_HOLD_MS` delay so an RTT viewer reconnecting after a
  silent reset has time to grab the cause line.

**Result (RTT, three successive boots):**
1. After flashing — modem AT all returned `0`, but the
   `--- LTE attach` log was never visibly printed. Heartbeats kept
   firing through 18+ s, so the kernel was alive — main was blocked
   inside `lte_lc_connect()`.
2. Manual reboot — modem probe again clean, then
   `--- LTE attach (NB-IoT, Telekom DE)` did print, then ~6 s of
   silence and a partial new line. **The device reset itself.** RTT
   dropped bytes around the reset.
3. After diag improvements — boot showed
   `*** RESET CAUSE BITMAP: 0x00000000 ***`,
   `<inf> diag: reset cause: 0 (cold start / cleared)`. Modem AT
   commands started, then long silence again, then heartbeats only.

**Diagnosis — brownout during modem RF activation:**
- On nRF91 the `RESETREAS` register has **no bit for cold POR or
  brownout** — both manifest as `cause = 0`. The chip's other reset
  causes (PIN, WATCHDOG, SOFTWARE, DEBUG, LOCKUP, …) all have their
  own bits and would be visible.
- Reset cause = `0` after a silent self-reset (no human power cycle,
  no debugger reset) is therefore the **signature of brownout** on
  this SoC. Not a software fault, not a watchdog, not a hardfault.
- The crash window is exactly when the modem moves from "boot + low
  power AT" into RF activity (`CFUN=1` / band scan). Modem boot pulls
  ~100–200 mA peak; LTE TX/scan can pull **~800 mA peak**.
- Confirms the user's earlier intuition that "the IC needs more caps"
  was on the right track.

**Verdict:** BLOCKED on power-supply rework. Software-side (modem
firmware + lte_lc + DT) is correct.

**Action items before this step can pass:**
1. **Bulk cap on VBAT**, low-ESR, 22–47 µF, placed close to the SiP
   VBAT pin per Nordic reference design.
2. **Verify the regulator feeding VBAT can supply ≥ 800 mA peak.**
   nPM1300 BUCKs are ~200 mA each — insufficient on their own. Either
   route VBAT directly from the battery / USB rail (with the bulk cap)
   or use the PMIC's VSYS passthrough.
3. **Scope VBAT during the silent reset** — expect to see a dip below
   the 3.0 V SoC minimum at the moment of RF activation. That's the
   smoking-gun confirmation.

**Optional software mitigation while hardware is reworked:**
- Lock the modem to a single NB-IoT band (Telekom DE → B8) before
  `lte_lc_connect()` to reduce scan-time current draw:
  `AT%XBANDLOCK=2,"10000000"`.
- Back off TX power class via `AT%XEMPR`.
These will not save a design that genuinely can't deliver the current,
but may let bring-up limp through for further validation.

---

## Step 5.1 — RF-vs-power isolation (AT+CFUN=4 soak)

**Goal:** Distinguish whether the silent reset in step 5 is triggered
by RF activation specifically, or by the modem's digital boot/draw.

**Method:** `AT+CFUN=4` puts the modem in flight mode — the digital
subsystem is fully booted and active, but the RF transmitter and
receiver are disabled. If the device survives this for a sustained
period, the silent reset must be caused by the RF coming up. If it
still resets, the supply can't even handle baseline modem draw.

**Changes:**
- `src/modem.{c,h}` — `modem_cfun4_soak(int seconds)`: sends
  `AT+CFUN=4`, queries `AT+CFUN?` to confirm, then loops sleeping 5 s
  at a time logging `CFUN=4 soak: N/M s elapsed (alive)` per tick.
- `src/main.c` — call `modem_cfun4_soak(60)` in place of
  `modem_lte_attach()` for this run. Attach is commented out (one
  uncomment to restore).

**Result (RTT, abridged):**
```
[00:00:07.286,315] <inf> modem: soaking in CFUN=4 for 60 s ...
[00:00:08.427,124] <inf> diag: heartbeat @ 8427 ms
[00:00:12.286,407] <inf> modem: CFUN=4 soak:  5/60 s elapsed (alive)
... heartbeats every 2 s, soak progress every 5 s, all clean ...
[00:01:07.287,322] <inf> modem: CFUN=4 soak: 60/60 s elapsed (alive)
[00:01:07.287,322] <inf> modem: CFUN=4 soak complete with no reset
```

**Verdict:** PASS for digital subsystem, **isolates the failure to
RF activation.** Heartbeats and soak ticks ran unbroken for the full
60 s. The supply can carry SoC + I²C/SPI activity + modem digital
boot + baseband — what it can't carry is what `CFUN=1` (or
`lte_lc_connect`) adds on top, namely the PA / RX front-end / band
scan currents.

**What this means concretely:**
- `RESETREAS = 0` after a non-user-initiated reset = brownout, and now
  we know it's **RF-activation-induced** brownout specifically.
- The fix path doesn't change (it's still hardware: bulk caps,
  regulator capability, antenna match) but the diagnosis is no longer
  speculative — we have a clean isolation experiment.
- A poor antenna match would worsen this (PA pulls more to keep EIRP
  on a mismatched load) but is not by itself the *mechanism*. The
  mechanism is supply UVLO.

---

## Step 5.x — Planned follow-ups (in order)

1. **Hardware rework (gated, blocks step 6):**
   - Add 22–47 µF low-ESR bulk cap on `VBAT` close to the SiP.
   - Verify `VBAT` source can supply ≥ 800 mA peak. If currently fed
     from an nPM1300 BUCK (200 mA), reroute to battery / USB / PMIC
     `VSYS` with the bulk cap.
   - Re-check the SiP's RF supply pin decoupling against Nordic's
     reference layout.
   - With a scope on `VBAT`, run `lte_lc_connect()` and capture the
     dip — that's the smoking-gun confirmation.

2. **Step 5.2 — Low-power attach attempt (software-only mitigation):**
   - Add `modem_lte_attach_lowpower()` that issues
     `AT%XEMPR` (TX power back-off) and
     `AT%XBANDLOCK=2,"10000000"` (lock to NB-IoT B8) **before**
     `lte_lc_connect()`.
   - Goal: pull less peak current and see if attach can limp through
     for further bring-up while the hardware fix is in flight.
   - Outcome possibilities: (a) attaches successfully → great, can
     proceed validating step 5; (b) still resets → confirms hardware
     fix is mandatory.

3. **Step 5.3 — Full LTE attach** (after hardware fix lands):
   - Restore the original `modem_lte_attach()` call.
   - Watch for CEREG transitions to `REGISTERED_HOME`/`ROAMING`.
   - Read `+CESQ` and `%XMONITOR` for signal quality.

4. **Step 6 onward** (post-cellular):
   - GNSS bring-up (antenna already connected).
   - Revisit IMU LSM6DSO (parked in step 2 — WHO_AM_I returned 0x00).
   - Revisit blue/green LEDs (parked in step 3 — only red worked).
   - Revisit the unconfirmed 0x0b I²C ACK (re-scan with 1-byte read).

---

## Step 5.2 — Low-power attach (software-only mitigations) — characterization done

**Goal:** While the hardware fix is in flight, see whether band-lock +
TX power back-off can squeeze the modem under the brownout threshold
enough to attach to NB-IoT.

**Changes:**
- `src/modem.{c,h}` — `modem_lte_attach_lowpower()`:
  - `AT+CFUN=0` (so the config commands aren't refused with +CME 518).
  - `AT%XBANDLOCK=2,<mask>` — runtime mask (mode 2, no NVM write).
  - `AT%XEMPR=0,0,2` — NB-IoT, all bands, 1.0 dB back-off (modem max).
  - Read back `%XBANDLOCK?` and `%XEMPR?` to confirm what stuck.
  - `lte_lc_register_handler()` + `lte_lc_connect()` blocking, with
    `+CESQ` / `%XMONITOR` log on success.
- `src/main.c` — call `modem_lte_attach_lowpower()` instead of the
  vanilla attach.

**Supply-rail characterization map (this step's main deliverable):**

| Configuration                  | Result                       |
|--------------------------------|------------------------------|
| All bands + 0 dB (default)     | Brownout immediately         |
| B8 + B20 + −1 dB               | Brownout within seconds      |
| B20 only + −1 dB               | Brownout within ~7–15 s      |
| **B8 only + −1 dB**            | **Stable 5+ min** (no cell)  |
| CFUN=4 (RF off)                | Stable indefinitely (5.1)    |

**Result (RTT, B8-only config):**
```
[00:00:07.190,338] <inf> modem: requesting NB-IoT attach (low-power, single band)...
[00:00:08.303,192] <inf> diag: heartbeat @ 8303 ms
... heartbeats steady every 2 s ...
[00:00:14.900,573] <inf> modem: reg status -> 4 (unknown)
[00:00:14.900,604] <inf> modem: cell update tac=0xffffffff cell_id=0xffffffff
... 5 minutes of heartbeats, no further reg events, no reset ...
```

**Verdict:** software margin is exhausted. B8-only at −1 dB is the
only configuration that stays alive long enough to do anything, and
even then no Telekom NB-IoT cell was visible on B8 at the test
location. Two indistinguishable possibilities for the "no cell" part:
1. Telekom NB-IoT in this spot is on B20, not B8 — but we can't open
   the bandlock to test without crashing (B20-only also crashes).
2. Coverage is poor at this location regardless of band, possibly
   compounded by antenna placement / match.

**Verdict:** BLOCKED on hardware. The modem subsystem and stack
(nrf_modem_lib, lte_lc, attach flow) are correct; the supply just
can't carry the current.

**Code parked at:** B8 only + −1 dB back-off (the only stable config).
Bandlock string can be swapped in `modem.c` for further experiments.

**Action items unchanged from step 5:**
- Bulk cap on VBAT (22–47 µF, low-ESR, close to SiP).
- Verify VBAT source can supply ≥ 800 mA peak.
- Scope VBAT during a CFUN=1 attempt to confirm the dip (will dip
  even harder when band lock is widened).
- Consider antenna match check too: poor VSWR worsens PA current and
  may explain why B20 (800 MHz) crashes faster than B8 (900 MHz) on
  the same supply.

**Once hardware is reworked, the test order is:**
1. Try `modem_lte_attach()` (full bands, full power). If it survives
   and attaches → step 5 PASS.
2. If it survives but doesn't attach → coverage / antenna issue, take
   the device to a known-good NB-IoT or LTE-M location to validate.

---

## Step 5.2 — devzone forum follow-up (draft)

Posted as a continuation of an earlier devzone comment about the power / nPM1300 / nRF7000 issues that preceded this bring-up. PPK2 captures referenced at the end will be edited in once they're up.

**Update — cellular bring-up findings (will add PPK2 traces below).**

Picking up from "now I'm working on testing the cellular modem". Quick recap of the journey so others searching for this don't have to repeat it:

1. First attempt at `nrf_modem_lib_init()` + `lte_lc_connect_async` crashed immediately with `<err> nrf_modem: Modem has crashed, reason 0xfff, PC: 0x0`. Turned out the chip was still on **PTI (Production Test Image)** firmware out of the box — that's the manufacturing-only firmware, it cannot do real LTE. Flashed `mfw_nrf91x1_2.0.4.zip` over SWD with the nRF Connect Programmer and the modem subsystem started behaving. `AT`, `AT%SHORTSWVER`, `AT+CGMR` all responded cleanly, so the firmware update was the first big unlock.

2. After that the modem boots fine but the device **silently resets a few seconds into `lte_lc_connect()`**. SEGGER RTT drops bytes around the reset so the failing reset's logs are lost. I added `hwinfo_get_reset_cause()` printed right at boot (mirrored via `printk` so it lands before the deferred LOG queue runs). After every silent reset, the next boot reports `RESET CAUSE BITMAP: 0x00000000`. On nRF91 the `RESETREAS` register has **no bit for cold POR or brownout** — both manifest as `cause = 0`. So a self-induced reset with `cause = 0` (no PIN, no WDT, no SOFTWARE bit) is essentially the brownout signature on this SoC.

3. To isolate RF vs digital draw, I set `AT+CFUN=4` (flight mode: modem digital subsystem fully active, RF disabled) and let it sit. It stayed up for 60+ seconds with periodic heartbeats. So the supply *can* carry the modem's baseline + I²C/SPI traffic — it can't carry whatever the modem does next when RF comes up.

4. Software-only attempts to limp through, with the supply margin map I ended up with:

   | Configuration | Result |
   |---|---|
   | All bands + 0 dB (default) | Brownout immediately |
   | `XBANDLOCK` B8+B20 + `XEMPR` -1 dB | Brownout within seconds |
   | B20 only + -1 dB | Brownout within ~7–15 s |
   | B8 only + -1 dB | Stable 5+ min, no cell found |
   | CFUN=4 (RF off) | Stable indefinitely |

5. Interesting tell: B8-only is stable but no cell visible (no TX activity → modem mostly RX → low current). B20-only crashes — and the most plausible explanation is that there's actually a faint Telekom NB-IoT cell on B20 in this location and the modem keeps doing **PRACH ramp-ups** trying to attach. Each PRACH burst is a TX spike pulling near-peak PA current, and on a borderline rail those spikes are what trip UVLO. Not the steady-state draw — the bursts.

So the conclusion is the supply just can't deliver the modem's ~800 mA peaks during RF activation, even at -1 dB back-off (which is the maximum `%XEMPR` allows). The hardware path I'm planning to fix is: bulk cap (22–47 µF low-ESR) right next to the SiP VBAT, and confirming the source feeding VBAT can actually deliver ≥800 mA peak (right now I'm fed straight from VBUS=5 V with the PPK2 inline; with the nPM1300 in the picture an individual BUCK is ~200 mA which obviously won't do).

PPK2 captures next — I have a B20-only -1 dB trace at 100 kS/s about to go up which should show the PRACH spikes right before the dropout, and I'll add a B8-only -1 dB trace as a control (expecting flat RX baseline, no spikes). Will edit them in below.

**Hardware around the nRF9151 VDD/VBAT (will attach annotated photo + gerbers):** the populated decoupling on the SiP supply path is:

| Ref | Part | Notes |
|---|---|---|
| C10 | Samsung CL05A106MP5NUNC (0402, 10 µF X5R 6.3 V) | local decoupling |
| C15 | Samsung CL05A106MQ5NUNC (0402, 10 µF X5R 6.3 V) | local decoupling |
| C14 | Samsung CL21A476MQYNNNE (0805, **47 µF** X5R 6.3 V) | bulk cap |
| C17 | HRE CGA0201X5R104K100ET (0201, 100 nF X5R 10 V) | high-freq bypass |
| FB2 | muRata BLM15AX121SN1D (0402, 120 Ω @ 100 MHz ferrite) | RF supply isolation |
| R44 | series jumper on the VBAT feed | originally desoldered (open); now **bridged with a solder blob** instead of a real 0 Ω part |

So the bulk side actually looks OK on paper (47 µF X5R close-ish to the SiP). What I'm not 100% sure about is R44 — bridging it with solder rather than a proper 0 Ω part is fine in principle but if there's any extra resistance there, an 800 mA TX peak would already drop ~mV per 10 mΩ, which adds up. Worth checking against the gerbers (also attached) to see whether the routing from the supply input through R44 to the bulk cap and the SiP is short and wide enough for those current peaks.

Anyone seen the same pattern on a custom 9151 board, or got recommendations for the supply topology beyond the obvious "follow the reference design"? In particular, opinions on the R44 solder bridge and whether the surrounding decoupling layout looks sane would be very welcome.


---

## Step 6 — nRF54L15 board bring-up: rename, strip, and the RTT-viewer rabbit hole

This step is in a separate sub-app under `nrf54l15/` so it doesn't disturb the 9151 work parked at the stable B8-only config. The L15 sits on the same custom palroc board next to the 9151 and shares some signals with it — most importantly nRESET (see below).

### 6.1 — board re-vendoring + naming

Started from a verbatim copy of Nordic's upstream `nrf54l15dk` board folder under `nrf54l15/boards/palroc/nrf54l15dk/`. Two-step rename to make it ours:

1. Flipped `vendor: nordic` → `vendor: palroc` in `board.yml`, so the build target becomes `palroc/nrf54l15dk/...` instead of `nordic/...`.
2. Renamed the board itself from `nrf54l15dk` to `nrf54l15_palroc` — directory move, all `nrf54l15dk_*` filenames bulk-renamed, and `BOARD_NRF54L15DK` → `BOARD_NRF54L15_PALROC` across `Kconfig*`, `board.cmake`, the `.dts`/`.yaml`/`_defconfig` files, the dts compatible strings, and the runner board lists in `board.yml`. Build target after rename: `palroc/nrf54l15_palroc/nrf54l15/cpuapp`.

### 6.2 — variant erase

Upstream Nordic ships seven variants of this board (L05/L10/L15 cpuapp, the `_ns` TF-M variants, plus the FLPR RISC-V coprocessor with and without XIP). We only need one. Deleted:

- L05 cpuapp (3 files: `.dts`, `.yaml`, `_defconfig`) — wrong SoC.
- L10 cpuapp (3 files) and L10 cpuapp_ns (3 files) — wrong SoC.
- L15 cpuapp_ns (3 files) — needs TF-M, big complication, not needed for bring-up. The 9151 needs `_ns` because the cellular firmware lives in the secure world and the app must be non-secure to talk to it; the L15 has no such constraint.
- L15 cpuflpr (3 files) and cpuflpr_xip (3 files) — RISC-V coprocessor variants; not used.
- `doc/` and `support/` (DK schematics + a JLink script for the FLPR core).

Then trimmed `board.yml` socs/runners, `Kconfig.nrf54l15_palroc` selects, `Kconfig.defconfig` (dropped the `_NS`/TFM block and a dead `SPI_NOR_FLASH_LAYOUT_PAGE_SIZE` block), and `board.cmake` (just `--device=nRF54L15_M33`).

### 6.3 — strip DK-only peripherals from the board dtsi

The DK common dtsi was full of stuff our custom palroc board doesn't have: 4 user LEDs (P2.9 / P1.10 / P2.7 / P1.14), 4 buttons, an `mx25r6435f` SPI flash on `spi00` + cs P2.5, the `nordic_expansion_header` connector, an expansion-side `spi22`, `pwm20` driving `pwm-led1`, `uart30`, and various aliases (`mcuboot-button0`, `mcuboot-led0`) that referenced those nodes. Everything got dropped — the board common dtsi is now down to just `&uart20` enable + a pinctrl include.

Renamed the awkward `nrf54l_05_10_15_cpuapp_common.dtsi` and `nrf54l15_palroc_nrf54l_05_10_15-pinctrl.dtsi` to drop the L05/L10 reference: now `nrf54l15_palroc_cpuapp_common.dtsi` and `nrf54l15_palroc-pinctrl.dtsi`. Pinctrl is reduced to a single `uart20` group (with a `FIXME` because the pins are still the DK's P1.4/5/6/7, not the palroc UART).

What stayed enabled in `cpuapp_common`: `cpuapp_sram`, `regulators` + `vregmain` (DCDC), `lfxo` / `hfxo` (configured below), `grtc` (kernel timer), `uart20`, `gpio0/1/2`, `gpiote20/30`, `clock`, `temp`, `radio`, `ieee802154`, `nfct`, `adc`, `gpregret1` + `boot_mode0`. A bring-up baseline matching the DK's set, minus the actual board peripherals.

### 6.4 — custom-board hardware deltas vs. the DK

Three things on the palroc board are different from the DK and one of them turns out to make the L15 unbootable by default:

- **No 32.768 kHz LFXO crystal on XL1/XL2.** The DK has one; we don't. With the LFXO node configured for an external crystal that isn't there, the kernel can hang waiting for LFXO startup. Fix in dts: `&lfxo { status = "disabled"; };` — GRTC then runs off the internal LFRC (~250 ppm, fine for bring-up; revisit if precise timing is ever needed).
- **HFXO is a SMD2016-4P, 32 MHz, CL = 8 pF, ±10 ppm, 50 Ω.** The DK common had `load-capacitance-femtofarad = <15000>` (15 pF, matching the DK's crystal). Updated to `<8000>` to match our crystal's load-cap spec; otherwise the chip sees the wrong tank capacitance and the crystal either fails to start or oscillates at the wrong frequency (which the radio cares about; GRTC still functions because it can fall back to HFINT).
- **No pull-up on nRESET. nRESET tied to nRF9151 P0.19.** No 10 kΩ to VCC means the L15's reset line is floating by default. The 9151's GPIOs come out of reset as input/disconnected (high-Z), so the 9151 isn't driving P0.19 either. Net result: nRESET sits low (leakage / capacitive coupling pull it down) and the L15 is held in reset *forever*, never executing a single instruction. This is a hardware design constraint of the phase-2 board — the 9151 is supposed to be the L15's reset master — but it means **the L15 cannot be evaluated standalone**.

Fix: hand-soldered a 10 kΩ from L15 nRESET to VCC on the dev board. (Software fix on the 9151 side — `gpio_pin_configure(gpio0, 19, GPIO_OUTPUT_HIGH)` very early in 9151 boot — would also work, and both together is the right answer for the production board: hardware pull-up gives a safe default, software drive lets the 9151 explicitly reset the L15 when needed.)

Saved this as a project memory (`project_palroc_l15_nreset.md`) so future sessions don't have to rediscover it.

### 6.5 — the diagnostic rabbit hole, and what actually broke

The factorial sample (Nordic's `lesson 4 exercise 2`) ran cleanly on the **DK hardware** built against the renamed `palroc/nrf54l15_palroc/nrf54l15/cpuapp` target, proving the rename + board files were fine. It produced **zero output** on the custom palroc board — across many config iterations.

I burned a lot of cycles chasing software ghosts on the assumption that the chip was running and only the log path was broken: re-adding `&clock` and other previously-stripped SoC nodes, fixing HFXO load cap, disabling LFXO, adding the explicit RTT prj.conf block (`CONFIG_USE_SEGGER_RTT=y` / `CONFIG_RTT_CONSOLE=y` / `CONFIG_LOG_BACKEND_RTT=y` / `CONFIG_UART_CONSOLE=n`), pristine-rebuilding many times. None of it helped — because none of it could help while the chip was held in reset.

The decisive diagnostic was firing up `JLinkExe` directly and asking the chip what it's doing:

```
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1
> halt
> regs   # PC = 0x000075B0, CycleCnt = 0x1E9, SP = 0x20002208 (valid SRAM)
> g
> halt
> regs   # PC = 0x000075B0, CycleCnt = 0x3E9 — only 0x200 cycles in run window
```

Two big takeaways:

1. **The chip is alive on SWD.** SW-DP ID `0x6BA02477`, Cortex-M33 r1p0 identified, VTref = 3.327 V (so power and the pull-up rework are good). Once the pull-up was on, the chip *was* booting — that part of the diagnosis was correct.
2. **PC is not in a polling loop, it's in `arch_cpu_atomic_idle`.** Running `addr2line` on `0x000075B0` against `build/zephyr/zephyr.elf` resolved it to `zephyr/arch/arm/core/cortex_m/cpu_idle.c:147` — the instruction immediately after the kernel idle thread's `WFE`. The 0x200 cycle delta between halts wasn't a tight spin, it was the chip woken once by a tick interrupt, doing a few instructions, and going back to sleep. That's textbook idle behaviour after `main()` reaches a `k_sleep()`.

So the firmware *was* running. Logs were being written. They just weren't getting *out*.

### 6.6 — RTT viewer auto-detect doesn't find the control block on L15

Symbol scan of `zephyr.elf` confirmed RTT is fully compiled in — `_SEGGER_RTT` symbol exists, `__init_rtt_init` sysinit exists, and the right `CONFIG_*RTT*` macros are baked into the build. The control block lives at:

```
arm-zephyr-eabi-nm zephyr.elf | grep _SEGGER_RTT$
20000470 B _SEGGER_RTT
```

The J-Link RTT Viewer's auto-detect didn't find it. The likely reason on nRF54L15 is the chip's TrustZone configuration: with secure debug enabled but the SAU treating part of SRAM as secure-only, the viewer's auto-search range may not cover (or may not have permission to read) the region where the secure linker placed `_SEGGER_RTT`.

**Fix: tell the viewer the address explicitly.** Once the connection dialog is set to "Address: 0x20000470" instead of "Auto detection", output starts streaming immediately.

The address can change between builds (linker may move it), so after every rebuild the lookup is:

```
arm-zephyr-eabi-nm build/.../zephyr/zephyr.elf | grep _SEGGER_RTT$
```

and the viewer needs to be reconfigured with the new address.

### 6.7 — current state and lessons

Where the L15 sub-app is parked:

- Board renamed to `palroc/nrf54l15_palroc/nrf54l15/cpuapp`, single SoC, single variant.
- DK-only peripherals removed from the dtsi; SoC-level enables retained.
- LFXO disabled (no crystal); HFXO load cap = 8 pF (SMD2016-4P spec).
- 10 kΩ pull-up on nRESET soldered. Long-term: also drive 9151 P0.19 high early in 9151 boot, and add a real pull-up on the next board spin.
- App is a minimal "alive heartbeat" — `LOG_INF("nRF54L15 palroc bring-up start")` then a `k_sleep(1s)` loop logging `alive t=N`. Output streams cleanly over RTT once the viewer is given the explicit `_SEGGER_RTT` address.

Lessons that are going to save someone else (or future me) hours:

1. **A custom-board chip that "doesn't work" isn't necessarily broken silicon, broken firmware, or a config issue — it might just be held in reset by an unconnected pin.** Always verify the chip is actually executing instructions before debugging anything else: `JLinkExe → halt → regs → g → halt → regs`. PC moving = alive. PC stuck = stuck. DPIDR readable but PC stuck at zero / vector table = held in reset / never booted.
2. **A reset line tied to another chip's GPIO is a hard cross-chip dependency** that's invisible from the L15-side software alone. Has to be documented prominently because the symptoms (silent failure, no logs) look identical to "broken board" or "broken firmware" from inside the L15.
3. **RTT auto-detect is not infallible on nRF54L.** When you've ruled everything else out and the chip is provably running, look up `_SEGGER_RTT` with `nm` and feed the address to the viewer manually before assuming the log path is broken.
4. **Stripping a Nordic reference board dtsi has a wide safe envelope as long as you keep the SoC-level nodes intact** (clock, regulators, gpio, gpiote, grtc, uart, sram). The DK-specific peripheral nodes (LEDs, buttons, mx25r64, expansion-spi, expansion-header) can all be dropped without consequence. What you *cannot* safely drop blind is a board-level hardware fact like "no LFXO crystal" — that requires an explicit `status = "disabled"` to override the SoC dtsi default.


---

## Step 7 — closing two parked items: LED MOSFETs + first nPM1300 telemetry

### 7.1 — blue/green LEDs: MOSFET package was bad, not the firmware

After step 3 only the red LED worked, with blue (P0.04) and green (P0.05) staying dark. Software side was identical for all three (same `gpio-leds` block, same `GPIO_ACTIVE_HIGH`, same `gpio_pin_set_dt` call in `leds_walk()`), and a multimeter showed the GPIOs going high on schedule, so it was clearly downstream of the SoC. The driving switching transistors are MOSFETs in a multi-channel package.

Root cause: **the MOSFET IC package was not making solid connections to the LED nets** — the part was placed but not actually conducting on the relevant channels, so the LEDs were effectively disconnected from VCC even with the GPIO driving its gate. Desoldered the offending package and the three LEDs all started behaving on the next power-up. Lesson saved with the chip-inventory memory: when a GPIO output measures right but the load doesn't react, suspect the switching transistor / its solder joints before doing anything to firmware. Worth a paste-mask / footprint review on the next board spin to prevent the assembly-yield issue from recurring.

### 7.2 — nPM1300 telemetry working end-to-end

We had a presence check on i2c2 since step 1 (ACK at 0x6b) but no real data. Now wired up the npm13xx charger driver via the Zephyr sensor API. The dts had to be precise — the binding requires `term-microvolt`, `current-microamp`, `dischg-limit-microamp` (only `200000` or `1000000` accepted), `vbus-limit-microamp`, `thermistor-ohms` (enum `0|10000|47000|100000`), and `thermistor-beta`. Omitting any of those makes the build fail at the dts-parse stage with a clear `'<prop>' is marked as required` message. Captured the full required-prop list in the npm13xx reference memory so this doesn't have to be rediscovered.

Kconfigs (driver was renamed in NCS 3.3.0 — `npm13xx` not `npm1300`):

```
CONFIG_MFD=y
CONFIG_MFD_NPM13XX=y
CONFIG_REGULATOR_NPM13XX=y
CONFIG_NPM13XX_CHARGER=y
```

`charging-enable` left off until an actual battery cell is profiled — we just want to read what's there.

First read had a stray garbled line right after the section banner (probably a partial dropped `LOG_INF` from the deferred logger competing with the fast i2c probe just before). Fix: synchronise the read to the 15-second boot mark with a `k_uptime_get()` + `k_msleep(target - now)` gate. Side benefit: the sample point is now predictable from a PPK2 trace, so power-vs-PMIC-state can be correlated.

First clean reading at the 15 s mark:

```
[00:00:14.413,940] <inf> diag: heartbeat @ 14413 ms
[00:00:15.004,760] <inf> npm1300: nPM1300 charger sample:
[00:00:15.004,821] <inf> npm1300:   VBAT   : 3.876000 V
[00:00:15.004,882] <inf> npm1300:   Tntc   : 23.059875 C
[00:00:15.004,943] <inf> npm1300:   IBAT   : 0.000000 A
```

VBAT = 3.876 V (consistent with a partial-charge LiPo or a regulator output), Tntc = 23.1 °C (so the 10 kΩ / β=3380 NTC is fitted and reads correctly — no more questions about whether to keep it in the dtsi), IBAT = 0 A (no draw in idle, expected). The whole gauge path (i²c → MFD → charger sensor driver → ADC trigger → channel get) works.

This unlocks more useful telemetry going forward: VBAT during modem TX bursts (would have made step 5's brownout debug noticeably easier), Tntc as a sanity check for the supply rework, IBAT to spot which subsystem is drawing what when. Worth adding to the modem-attach loop as periodic background logging once the supply rework is done.

### 7.3 — what's still parked

- **LSM6DSO IMU returns 0x00** on `WHO_AM_I` — software path looks right (mode 0, CS=P0.16, bit 7 set on read register). Likely candidates next time: CS not actually toggling under the scope, IMU VDD not enabled, or some 1.8 V vs 3.3 V mismatch on the supply.
- **0x0b I²C ACK from step 1 unconfirmed** — disappeared in step 2 and was tentatively classified as a phantom from the zero-byte probe. Now that the L15 is provably booting (with its nRESET pull-up), a re-scan should make this either consistently present (= it's the L15) or consistently absent (= confirmed phantom). 5-minute test next time.
- **9151 supply rework** for full LTE attach — gerber + bulk-cap-near-SiP work, blocked on hardware iteration, not software.
- **L15 BLE beacon** — visible on DK, not on custom board. PPK2 trace looks like ~no current change after flash, so either `bt_enable()` is failing early (possibly HFXO load cap still wrong) or it's running but the radio is off-frequency. Quick win when picked up: JLinkExe halt + `mem 0x20000470 64` to read the RTT control block contents directly and see what `LOG_INF` actually got written.


---

## Step 8 — second board attaches to a cell, four parked items close, band-sweep tool added

A clean attach on a second physical palroc board with the same software config that brownouted board #1 in step 5.2 — proves the brownout was supply-rail-bound on board #1, not a fundamental modem/firmware problem. The actual log:

```
[00:00:07.327,178] <inf> modem:   band lock OK
[00:00:07.327,880] <inf> modem:   TX back-off OK
[00:00:07.329,956] <inf> modem: requesting NB-IoT attach (low-power, single band)...
[00:00:15.751,739] <inf> modem: reg status -> 2 (searching)
[00:00:15.751,770] <inf> modem: cell update tac=0xaa59 cell_id=0x6903124
[00:00:15.936,340] <inf> modem: RRC connected
...several RRC connected/idle cycles + heartbeats...
[00:01:24.473,388] <inf> modem: reg status -> 5 (registered (roaming))
[00:01:24.473,480] <inf> modem: connected in 77s
[00:01:24.474,060] <inf> modem: +CESQ: 99,99,255,255,25,42
[00:01:24.474,822] <inf> modem: %XMONITOR: 5,"","","21403","DE00",9,20,"06B05398",209,6154,42,31,...
```

Decoded `%XMONITOR`:

| Field | Value | Meaning |
|---|---|---|
| nw_reg_status | 5 | registered (roaming) |
| PLMN | 21403 | MCC 214 / MNC 03 → Orange España (Telekom DE roaming partner) |
| TAC | DE00 | tracking area |
| AcT | 9 | NB-IoT |
| Band | 20 | matches the runtime bandlock |
| Cell ID | 06B05398 | |
| RSRP (raw) | 42 | ≈ -116 dBm (weak indoor) |
| RSRQ (raw) | 31 | ≈ -19 dB (borderline OK) |

The signal is weak but the modem holds the link through several RRC cycles before final registration at 77 s. **Software config is validated end-to-end** on healthy hardware.

### What this closes

1. **Board #1 brownout = supply-rail issue, confirmed.** Same software, same SIM, same band-lock + back-off — board #2 attaches, board #1 doesn't. The R44 solder bridge / bulk cap / VBAT path on board #1 is the next thing to rework, exactly as the devzone draft hypothesised. Step 5.2's characterisation map (B8-only stable, B20-only brownout, B8+B20 brownout) was a board-#1-specific power story, not a band story.
2. **`0x0b` I²C ACK is confirmed phantom.** Re-scanned with the L15 also alive on the board (its nRESET pull-up was added in step 6.4). The scan only sees `0x6b` (PMIC). The single 0x0b hit in step 1 was a zero-byte-probe artefact on the nRF TWIM, not the L15. The chip-inventory memory has been updated to reflect this.
3. **Flash regression after the npm1300 driver was added — fixed.** `flash JEDEC: ef 40 18 PASS` is back. Root cause: enabling `CONFIG_REGULATOR_NPM13XX` and adding the `regulators { BUCK1 {}; BUCK2 {}; LDO1 {}; LDO2 {}; }` children let the regulator framework gate one of those rails during init, killing the SPI flash + IMU's VCC. Fix: add `regulator-always-on` + `regulator-boot-on` to all four children, telling Zephyr to leave the PMIC in whatever state it powered up in. Saved as a reference memory so this doesn't bite again.
4. **nPM1300 telemetry on board #2 is junk** (`VBAT 0.019 V`, `Tntc -88 °C`, `IBAT 0 A`). Not a software bug — board #2 has no battery on BATSNS and no NTC on the thermistor pin, so the PMIC's ADC reads floating inputs. The PMIC itself responds (no I²C error, sample completes). Confirms board #1's earlier 3.876 V / 23.06 °C reading was real, and means the dts profile (`thermistor-ohms = <10000>; thermistor-beta = <3380>`) is fine — the boards just have different population.

### What's still parked

- **LSM6DSO IMU returns `0x00`** on `WHO_AM_I` on **both** boards even after trying SPI mode 3 and a throw-away first read with a 10 ms gap. Two independent boards with the same failure mode is conclusive: this is a board-level hardware issue, not a firmware issue. Most likely a footprint/package problem (the LED MOSFET precedent is right there) or a CS-line invert/buffer in the schematic. Decision: **move LSM6DSO to i2c2 on the next board revision** rather than chase the SPI wiring further. We have plenty of other things to debug; the IMU data path can wait until v2.

### New tool: `modem_band_sweep()`

Added a small band-sweep helper to `src/modem.c` (`void modem_band_sweep(void)`, declared in `modem.h`) that takes a list of LTE bands, locks each one in turn via `AT%XBANDLOCK=2,"<bitstring>"`, fires `lte_lc_connect_async`, and waits up to 90 s per band for `LTE_LC_NW_REG_REGISTERED_HOME/ROAMING`. Logs success time + `+CESQ` / `%XMONITOR` on attach, or a timeout warning otherwise. Useful when characterising a new SIM or location without hand-editing bitstrings.

Default candidate list: `{1, 3, 8, 20, 28}`. Edit the `bands[]` array at the top of the function in `modem.c` to widen / narrow it. The helper `band_to_bandlock_string()` constructs the right-aligned 88-bit bandlock string for any single band 1..88, so adding new bands to the list doesn't require redoing bit-counting by hand.

In `main.c` the sweep is **off by default** — there's a commented-out call right under the existing `modem_lte_attach_lowpower()` line. To run it: comment out the single-band attach, uncomment the sweep block, rebuild + flash. The sweep parks the modem in `CFUN=4` at the end so it doesn't leave RF active afterwards.


---

## Step 9 — nPM1300 BUCK rail mapping mistake; nRF7000 supply requirements

### 9.1 — the wiring mistake

Custom palroc board routes the nPM1300's two BUCK outputs to:

| nPM1300 rail | Routed to | Original design intent |
|---|---|---|
| BUCK1 (`VOUT1`) | nRF7000 **VBAT** pin | 1.8 V |
| BUCK2 (`VOUT2`) | nRF7000 **VDDIO** + 3V3 rail (W25Q128JV NOR, LSM6DSO, etc.) | 3.3 V |

The intent (BUCK1=1.8 V / BUCK2=3.3 V) **violates a hard nRF7000 datasheet constraint**: `VBAT ≥ VDDIO`. With VBAT < VDDIO the chip will not work safely, and depending on the operating window can also be damaging long-term.

### 9.2 — software-only fix on the existing board: invert the rail voltages

We can flip the two voltages without rewiring anything physical, as long as we respect every load on each rail:

| Rail | New voltage | Constraints satisfied |
|---|---|---|
| BUCK1 → nRF7000 VBAT | **3.3 V** | nRF7000 VBAT max well above this; VBAT − VDDIO = 0.6 V margin |
| BUCK2 → nRF7000 VDDIO + 3V3 loads | **2.7 V** | W25Q128JV NOR flash spec'd 2.7–3.6 V (so 2.7 V is its floor); LSM6DSO 1.71–3.6 V (fine); nRF7000 VDDIO 1.8 V or 2.7 V both valid (datasheet) |

Applied this in the dts via `regulator-init-microvolt` + matching min/max on BUCK1 and BUCK2:

```
npm1300_buck1: BUCK1 {
    regulator-init-microvolt = <3300000>;
    regulator-min-microvolt  = <3300000>;
    regulator-max-microvolt  = <3300000>;
    regulator-always-on;
    regulator-boot-on;
};
npm1300_buck2: BUCK2 {
    regulator-init-microvolt = <2700000>;
    regulator-min-microvolt  = <2700000>;
    regulator-max-microvolt  = <2700000>;
    regulator-always-on;
    regulator-boot-on;
};
```

Setting min == max == init pins the rail and prevents the regulator framework from deciding to roam. After flashing, **measure both rails with a multimeter before connecting the nRF7000 to anything** — confirm BUCK1 ≈ 3.3 V and BUCK2 ≈ 2.7 V *before* powering up the Wi-Fi chip on the new firmware.

### 9.3 — nRF7000 must do passive scan only

nPM1300 BUCKs are current-limited to ~250 mA per channel. nRF7000's active scan (transmits probe requests on each channel, listens for probe responses) draws peak currents that exceed that — same class of brownout we already saw on the 9151 modem in step 5.

**Constraint locked in for this board**: when the nRF7000 driver/app code is added, scan_type must always be `WIFI_SCAN_TYPE_PASSIVE`. Active scan would brownout BUCK1 and at best fail to attach, at worst cause supply transients that cascade across the system. Passive scan (listen only for beacons) draws far less and stays within the BUCK current envelope.

We can revisit when:
- The next board spin combines BUCK1+BUCK2 in parallel (some nPM1300 configs allow up to ~400 mA combined — read the datasheet carefully)
- VBUS / VBAT-passthrough is wired directly to nRF7000 VBAT (skipping the nPM1300 entirely on the high-current path)
- A different PMIC topology is chosen for nRF7000 supply

Until any of those happen, **passive only**, no exceptions.

### 9.4 — lessons for the next board spin

1. **Always cross-check supply rail voltages against every load's `(VBATmin, VBATmax) ∩ (VDDIOmin, VDDIOmax)` envelope, plus any cross-supply ordering constraints** like nRF7000's `VBAT ≥ VDDIO`. These are buried in datasheet sections labelled "Supply rules" or "Power-up sequencing" and are easy to miss if you only look at the absolute min/max.
2. **A 0.6 V margin between staircased rails is a comfortable default** when the constraint is `VA > VB`; some chips want 0.3 V minimum, others want a 1:1 ratio. Read the specific chip docs.
3. **PMIC current limits are a hard constraint per channel, not a "design suggestion"**. nPM1300 BUCKs at 250 mA can't carry an 800 mA Wi-Fi TX peak (or a 600 mA NB-IoT peak — see step 5.2 for the modem-side version of the same lesson). For high-current loads, plan VBAT-passthrough or a beefier PMIC up front.
4. **Document every cross-chip supply dependency in a per-rail table early** — spending an hour on this before tape-out would have caught the BUCK1/BUCK2 inversion before parts were ordered. Saved as a project memory so future bring-ups have the topology + scan constraints captured up front.


---

## Step 10 — GNSS bring-up (COEX0-gated LNA on nPM1300 LDO1)

The 9151 has a built-in GNSS L1 receiver (GPS / GLONASS, ~1565–1586 MHz). To get usable indoor / weak-signal performance the palroc board includes an external **LNA on the GNSS antenna feed**, with two design constraints worth capturing:

1. **The LNA is powered from nPM1300 LDO1.** LDO1's `regulator-always-on` flag in the dts keeps the rail energised, but the LNA itself only draws current when…
2. **…the modem's COEX0 line is asserted, which gates the LNA**. The 9151 drives COEX0 high during GNSS RX windows, low otherwise — so the LNA is only consuming power when GNSS is actually receiving. Configured at runtime via `AT%XCOEX0=1,1,1565,1586` (mode=1 enable, gnss_pin=1 active-high, frequency window 1565..1586 MHz = GPS L1 / GLONASS L1).

### What's added

A new module `src/gnss.c` (`gnss_probe(int duration_seconds)`, declared in `gnss.h`) handles the whole sequence:

1. `AT%XCOEX0=1,1,1565,1586` → enable LNA gating
2. `AT+CFUN=31` → switch to **GNSS-only** mode (LTE off; cleanest possible test, no risk of TX competing for the antenna front-end or causing supply transients during the run)
3. `nrf_modem_gnss_event_handler_set(...)` + `nrf_modem_gnss_fix_interval_set(1)` + `nrf_modem_gnss_fix_retry_set(0)` → continuous 1-Hz tracking, no retry deadline
4. `nrf_modem_gnss_start()` → go
5. **Every 5 seconds**: log `tracked` (count of SVs the receiver is decoding), `in_fix` (count being used in the position solution), `max_cn0` (best signal among tracked SVs in dB-Hz), and `fix=YES/no`. On a fix, also dump lat/lon/alt/accuracy.
6. End: `nrf_modem_gnss_stop()`, `AT+CFUN=0`, summary line with totals (PVTs received, max tracked, best CN0).

### How to enable

In `src/main.c` flip the toggle:

```
#define RUN_GNSS_PROBE       1
#define GNSS_PROBE_DURATION  60
```

The probe runs *between* `modem_probe()` and the LTE attach/sweep, so it happens before any LTE state, and the modem returns to `CFUN=0` at the end of the probe so the LTE phase that follows starts clean.

### What "working" looks like

- **Indoor / window**: 4–8 SVs tracked, max CN0 in the 25–40 dB-Hz range, fix often within 30–90 s if the antenna has any sky view. Below 25 dB-Hz signals are usually too weak to lock.
- **Outdoor / clear sky**: 8–12 SVs, max CN0 35–48 dB-Hz, fix in 30–60 s cold start (faster with assistance / hot start).
- **No LNA / LNA not powered**: max CN0 typically <20 dB-Hz, no fix even outdoors. If the COEX0 line isn't asserting (e.g., the AT%XCOEX0 command failed) or LDO1 isn't actually feeding the LNA, the receiver will see roughly the antenna's bare gain — strong enough to detect satellites occasionally but not to track and decode reliably.

### Diagnostic value if it doesn't work

If the GNSS probe runs but tracks zero satellites or has all-zero CN0:

- **Multimeter on LDO1 output**: should be at the LNA's expected supply (typical 1.8 V or 2.7 V depending on the LNA part — set explicitly via `regulator-init-microvolt` if needed).
- **Multimeter / scope on COEX0 pin** (P0.13 group on 9151 — check the schematic for the actual COEX0 routing) during the probe: should pulse high when the GNSS RX window opens.
- **Antenna connectivity**: continuity check from the antenna feed to the LNA input.
- **Sky view**: GNSS will not lock indoors with no window. Run the probe with the board at a window or outside before concluding the path is broken.

### Project memory

The supply topology + COEX0 gating pattern is already covered by the `nRF7000 supply + passive-scan` memory (LDO1 is mentioned indirectly there). Specific GNSS-LNA constraints can be added as follow-up if a future-me forgets the COEX0 spell.


---

## Step 11 — GNSS path validated, marginal signal, A-GNSS roadmap

### 11.1 — what worked, what didn't

After flipping `RUN_GNSS_PROBE=1` and pristine-rebuilding, the first run gave **zero satellites tracked** outdoors with clear sky. AT readback (`AT%XCOEX0?` returning `1,1,1565,1586`) confirmed the modem was driving COEX0 correctly, so the problem was downstream — between COEX0 and the LNA enable.

**Root cause: voltage-level mismatch on the LNA enable pin.** The 9151's GPIO output supply (VDDIO_GPIO) is fed from BUCK2, which in step 9 we pinned at **2.7 V** to satisfy the nRF7000's `VBAT > VDDIO` constraint. So COEX0's logic-high level is 2.7 V, not 3.3 V. The LNA's VEN spec is `max VEN ≤ VCC` and `min HIGH = VCC − 0.3 V`. We had LDO1 (powering the LNA) at the PMIC default — 3.0 V or 3.3 V depending on chip variant. With VCC = 3.3 V, the min-HIGH threshold is 3.0 V, and COEX0 only delivers 2.7 V. **The LNA never enabled.**

The fix is to pin LDO1 at exactly the same voltage COEX0 swings to:

```
npm1300_ldo1: LDO1 {
    regulator-init-microvolt = <2700000>;
    regulator-min-microvolt  = <2700000>;
    regulator-max-microvolt  = <2700000>;
    regulator-always-on;
    regulator-boot-on;
};
```

With that, LDO1 = 2.7 V, LNA VCC = 2.7 V, LNA VEN min-HIGH = 2.4 V, COEX0 high = 2.7 V → clean above threshold and within `max VEN ≤ VCC`. After this change, satellites started appearing — peak observation was `best_cn0 = 41 dB-Hz` (strong outdoor signal), confirming the LNA path is fully alive when the levels match.

**Saving this lesson for next board spin and future bring-ups:** when a chip's enable pin is driven by a GPIO from a different chip, **the GPIO's logic-high level must satisfy the receiving chip's min-HIGH threshold**. Easy thing to overlook when both rails happen to "look 3.3-V-ish" on the schematic. Two-rail systems (like our BUCK1=3.3 V / BUCK2=2.7 V split) compound this — the chip's GPIO output level depends on which rail feeds VDDIO_GPIO, not on whatever the design intent was. Cross-check every cross-chip enable line against the actual GPIO supply and the receiver's logic-level spec.

### 11.2 — current state: validated path, marginal signal

A 5-minute outdoor run after the LDO1 fix:
```
=== GNSS probe complete: 300 total PVTs, max_tracked=12, best_cn0=28.9 dB-Hz ===
```

**12 distinct PRNs tracked** but `best_cn0 ≈ 29 dB-Hz` typical (one earlier run hit 41 dB-Hz briefly). **No fix achieved.** The receiver is healthy (300 clean 1-Hz PVT events over the run, GNSS subsystem nominal), but signal quality is right at the threshold for navigation message decode (~30 dB-Hz needed for clean ephemeris, sustained for ~30 s per SV) and isn't sustained. All tracked SVs reported `el=0, az=0` — almanac/ephemeris hasn't been decoded, so the receiver knows what PRN signatures it correlates against but doesn't know where they are in the sky.

**What's validated, even without a fix:**

- ✓ LDO1 at correct voltage (2.7 V to match COEX0)
- ✓ COEX0 driving the LNA's VEN successfully (SVs only appear when this works)
- ✓ Antenna → LNA → modem RF chain is intact (the receiver is correlating real signal)
- ✓ 9151 GNSS subsystem operating cleanly (1 Hz PVT cadence, stable for 5+ min)
- ✓ Receiver scanning the full constellation (12 different PRNs detected, including some QZSS)

**What's not yet sufficient for production-grade fix:**

- ✗ Sustained CN0 >30 dB-Hz across 4+ SVs simultaneously
- ✗ Antenna gain pattern verified across the full sky
- ✗ Pre-LNA SAW filter (ambient WiFi/LTE energy can desensitise the LNA without one)

The signal-quality gap is most likely an **antenna / RF-front-end** issue rather than a firmware issue. The earlier 41 dB-Hz observation proves the path *can* deliver strong signal; sustained marginal CN0 across many SVs is a "the antenna and front-end aren't quite matching the chip's noise figure" story.

### 11.3 — A-GNSS roadmap (three increasing levels)

If we want fix performance to stop depending on whether we happen to see strong sky-view satellites, the answer is **assistance** — feeding the modem time/position/ephemeris information from outside so it doesn't need to decode it from marginal signal.

**Level 1 — LTE-time-only (free, ~10 min of work, modest improvement).** Run GNSS in `CFUN=1` (LTE+GNSS) instead of `CFUN=31` (GNSS-only). The modem auto-syncs UTC time from the LTE network and uses it as a code-phase hint. Doesn't help ephemeris decode but tightens the search space. Implementation: replace the `AT+CFUN=31` in `gnss.c` with whatever brings the modem to full functionality (`AT+CFUN=1` after `AT%XSYSTEMMODE` includes both LTE and GNSS — already true after step 10's `LTE_NETWORK_MODE_NBIOT_GPS` Kconfig change). Caveat: LTE will be active and drawing current during the test, so PPK2 traces look different.

**Level 2 — Manual coarse-position injection (free, ~30 min of work, moderate improvement).** Hardcode "I'm in Spain near Madrid at approximately lat=40.4°, lon=-3.7°" via `nrf_modem_gnss_agnss_write()` with `NRF_MODEM_GNSS_AGNSS_LOCATION`. Combined with LTE time from Level 1, the receiver now knows where *and* when it is, so it can compute exact Doppler offsets for each visible SV. Massively shrinks cold-start search time. Still doesn't help marginal-signal ephemeris decode, but the receiver gets to "I see SV X at frequency Y" much faster.

**Level 3 — Full A-GNSS via nRF Cloud (real fix in marginal conditions, ~2-4 hr of work, dramatic improvement).** Setup an nRF Cloud account (free tier exists, https://nrfcloud.com), provision the device with a JWT or X.509 cert, link the `nrf_cloud_agnss` library. The cloud sends back **decoded ephemeris** for currently visible SVs — the receiver no longer needs to demodulate the navigation message from weak signal, it just needs to lock the carriers. **This is what actually solves the 28.9 dB-Hz problem.** Sub-30 s fix even at marginal CN0. Cost: device provisioning is a real bring-up subtask of its own (cert generation, cloud config, dependency Kconfigs).

### 11.4 — recommended next steps

1. **Park the GNSS path as "validated, marginal signal" for now.** The bring-up is genuinely complete in the sense that every chain element is verified working. Production-grade fix performance is a signal-quality problem that's properly solved at hardware (better antenna, SAW filter, RF layout) or cloud-assistance level, not by more firmware iteration.
2. **For next board spin, add a SAW filter** between the antenna and LNA. Standard nRF reference design includes one; ours doesn't. This typically buys 5-10 dB of usable dynamic range by keeping cellular/WiFi out of the LNA front-end.
3. **When time allows, integrate Level 3 A-GNSS via nRF Cloud.** Once that's in place, the current 28.9 dB-Hz signal would be plenty for a fix and we no longer care about antenna pattern variability.
4. **Don't change the LDO1=2.7 V pinning** unless the level-shifting analysis is redone. It's load-bearing — the LNA only enables because LDO1 matches COEX0.

### 11.5 — module reference

`src/gnss.c` provides:
- `gnss_probe(int duration_seconds)` — the bring-up probe used here
- COEX0 enable via `AT%XCOEX0=1,1,1565,1586`
- `AT%XCOEX0?` readback for verification
- Periodic per-5s log: tracked count, in_fix count, max CN0, fix state, plus a per-SV dump (PRN / CN0 / elevation / azimuth / used/healthy flags)
- Exit: `nrf_modem_gnss_stop()` + `AT+CFUN=0`

Toggle in `src/main.c`:
```
#define RUN_GNSS_PROBE       1
#define GNSS_PROBE_DURATION  300
```

Whenever the A-GNSS roadmap is picked up, `gnss.c` is the file to extend; the CFUN sequencing and event-handler scaffolding is already in place.


---

## Step 12 — flash regression saga, charging validation, SPI3 parked as HW issue

### 12.1 — supply-rail voltage and the nPM1300 100 mV step gotcha

After step 9 set BUCK2 to 2.7 V (the W25Q128JV's absolute minimum spec), the SPI flash JEDEC reads regressed from `EF 40 18` (PASS at 3.3 V default) to `00 00 00` (FAIL at 2.7 V). Tried bumping BUCK2 progressively:

| Attempted | Actually applied (multimeter) | Why |
|---|---|---|
| 2.85 V | **2.71 V** (rejected) | nPM1300 BUCK voltages are **100 mV steps from 1.0 to 3.3 V**. Off-grid values like 2.85 V are silently rejected by the regulator framework — the driver doesn't error, the chip just falls back to its OTP default (2.7 V on this part). Saved as a feedback memory so future bring-ups don't lose hours to this. |
| 2.8 V | 2.8 V (pristine rebuild required) | Valid step. Initial test failed because of stale build cache; multimeter still showed 2.7 V until a real `rm -rf build_2 && west build` cycle. |
| 2.9 V | **2.9 V (confirmed by multimeter)** | Valid step, dts applied cleanly, 200 mV above flash min. **Flash still fails 3/3 with `00 00 00`.** |

**Conclusion: it's not a supply margin issue.** With BUCK2 confirmed at 2.9 V — well above the W25Q128JV's spec floor — the flash is still returning all-zero JEDEC. Voltage is exonerated.

### 12.2 — both SPI3 devices fail identically → board-level HW issue

Putting two parked symptoms next to each other:

| Chip | CS | Result | Across boards |
|---|---|---|---|
| W25Q128JV NOR flash | P0.12 | `JEDEC = 00 00 00` consistent across 3 retries | Was working at 3.3 V default; broke after BUCK2 pinned at 2.7 V; doesn't recover at 2.9 V |
| LSM6DSO IMU | P0.16 | `WHO_AM_I = 0x00` | Failing on **both** physical boards |

Both devices on **the same SPI3 bus** (SCK=P0.13, MOSI=P0.14, MISO=P0.15) returning identical all-zero reads is a strong signal that the failure is on the **bus side**, not the device side. The most likely root causes ranked:

1. **Bad solder joints on one of the SPI3 nets** — the LED MOSFET precedent has us assuming nothing about assembly quality on this board. MISO is the prime suspect because both chips depend on it, and a broken MISO trace would show exactly this pattern (clocks go out fine, both CS pulses register at the chips, but their responses never reach the SoC). Could also be SCK or MOSI broken — same symptom.
2. **Flash chip package itself has bad joints** specifically (different chip from the same regression — possible since flash *was* working at 3.3 V before, package can develop a marginal joint that's voltage-sensitive). Less likely given two devices fail identically on the same bus.
3. **9151 SPI3 pin-mux not actually configured** despite our pinctrl. Unlikely since the same pinctrl block has been working across all sessions; we'd need to scope the lines to confirm.

**Without a scope on the SPI3 lines, no further progress is possible from firmware.** The diagnostic is fundamentally hardware-side now.

### 12.3 — charging path: validated, then disabled

While the flash story was unfolding, we did get end-to-end validation of the nPM1300 charger:

```
[t=15s] <inf> npm1300: nPM1300 charger sample:
[t=15s] <inf> npm1300:   VBAT   : 3.881000 V → 3.950000 V  (climbing during run)
[t=15s] <inf> npm1300:   Tntc   : 21.34 °C    (NTC fitted, in spec)
[t=15s] <inf> npm1300:   IBAT   : 0.157807 A  (matches our 150 mA `current-microamp` setting)
```

That proves:
- VBUS detection working
- npm13xx charger driver enabling CC charging via dts `charging-enable`
- Charge current matches the 150 mA target (158 mA measured ≈ 5 % within tolerance — perfect)
- VBAT actually climbing (cell accepting charge)
- NTC reading correctly during charge — important for thermal protection

Charging then **temporarily disabled** (commented `charging-enable` in the dts) as part of variable-elimination during the SPI debug. **Re-enable it once the SPI3 issue is sorted** by uncommenting that single line in the `npm1300_charger` node.

### 12.4 — what's parked, what's locked in

**Parked, blocked on hardware:**
- W25Q128JV flash on SPI3 (CS=P0.12). Returns `00 00 00`. Suspect SPI3 bus solder joint or MISO trace.
- LSM6DSO IMU on SPI3 (CS=P0.16). Returns `0x00`. Same root cause class.
- Decision: **scope SPI3 lines (SCK, MOSI, MISO) during a JEDEC read to identify which net is broken**, then rework the affected joint. Until then, both SPI3 devices stay parked. Next board spin should also move the LSM6DSO to i²c2 per step 8 — that's already on the list.

**Locked in and validated:**
- BUCK2 = 2.9 V (multimeter confirmed) — flash margin no longer an issue, and nRF7000 VBAT − VDDIO = 0.4 V which still satisfies VBAT > VDDIO.
- LDO1 = 2.9 V — keeps the COEX0-vs-LNA-VEN level-shifting maths correct.
- Charging path validated at 158 mA, currently disabled in dts but easily re-enabled.
- Multimeter is now a mandatory step in the regulator-voltage workflow on this board, not an optional one.

### 12.5 — feedback lessons

1. **nPM1300 BUCK and LDO voltages are quantized to 100 mV steps.** Off-grid values like 2.85 V are silently rejected — the regulator framework returns no error and the chip stays at its previous (often OTP-default) value. Always check the multimeter after a voltage change, and if it reads "wrong but suspiciously round" (e.g., 2.7 V default), suspect the request was off-grid. Saved as a feedback memory.
2. **Pristine rebuild is mandatory after dts voltage edits.** `west build` without `--pristine` (or `rm -rf build_2`) caches the previous dts and silently keeps the old voltage on the chip. Confirmed via multimeter that *every* in-session voltage rebuild needed an explicit clean.
3. **Two devices on the same bus failing identically is a bus-side diagnostic.** Don't keep iterating on per-device fixes — find the shared trace (MISO is most often the culprit because both chips drive it back) and scope it.


### 12.6 — board #1 confirms it: different garbage pattern, same root class

After parking step 12 on board #2, ran the same firmware on board #1 just to see what its SPI3 path does. Different garbage:

```
[00:00:15.105,712] flash JEDEC attempt 1/3: 00 00 17 (unexpected; will retry)
[00:00:15.115,905] flash JEDEC attempt 2/3: 17 17 17 (unexpected; will retry)
[00:00:15.126,098] flash JEDEC attempt 3/3: 17 17 17 (unexpected; will retry)
[00:00:15.246,551] imu WHO_AM_I (0x0F) = 0x17 (expected 0x6C for LSM6DSO)
```

Board-by-board comparison (same firmware, both running BUCK2=2.9 V verified by multimeter):

| Board | Flash JEDEC reads | IMU WHO_AM_I | Pattern |
|---|---|---|---|
| #2 | `00 00 00` consistent | `0x00` | MISO appears low/floating, no chip response getting through |
| #1 | `17 17 17` (after first byte settles) | `0x17` | MISO stuck at a non-zero corrupted pattern, both chips read identical |

**Two key inferences:**

1. **Same fingerprint within a board, different fingerprints across boards.** Both chips on the same SPI3 bus return identical bytes, on each board — that's the textbook signature of a MISO-side fault rather than per-chip damage. The bus carries some specific corrupt state, the CS lines are working (toggling correctly per chip), and the SoC reads back the bus state regardless of which chip was addressed. If it were per-chip damage, the two chips on the same board would fail differently.
2. **The corruption pattern is board-specific.** Board #2 reads zeros, board #1 reads a fixed `0x17`. If the firmware were the cause, both boards would fail identically. Different patterns = assembly defects unique to each unit. This is the same class of issue as the LED MOSFETs (board-#1-specific), the LTE-attach brownout (board-#1-specific supply rework needed), and now SPI3 (different specific failure on each board).

The on-attempt-1 progression `00 00 17 → 17 17 17 → 17 17 17` is also informative — the first read shows the bus settling, then steady-state corruption locks in. Suggests a **marginal solder joint** that needs some clock activity to "wake up" into its broken-but-stable state. Cold solder joint or partial open is the textbook explanation.

**Updated parked diagnosis (no change in conclusion, more evidence):**

- SPI3 is broken on both physical boards, in different ways.
- Voltage is exonerated (BUCK2 = 2.9 V verified by multimeter on both boards).
- SPI mode is exonerated (mode 0 on both flash and IMU, matching what was working originally).
- **Next-session action: scope MOSI / MISO / SCK on each board during a `spi_flash_jedec()` call**, comparing against a DK running the same firmware. The line that's different from the DK's signal is the broken one. Then visually inspect that net's solder joints under magnification and reflow.

Closing note: having two-boards-different-garbage is paradoxically *good* diagnostic news — it confidently rules out firmware error and design error, and localises the problem to assembly. The fix is rework, not rewrite.


### 12.6.5 — important reframing: SPI3 contention may have *damaged* the slaves, not just corrupted reads

A user-driven insight after the day's debugging settled, worth capturing because it changes the diagnosis from "SPI3 bus is contested" to "SPI3 slaves were *electrically destroyed* by contention":

**Timeline that fits the new hypothesis:**

| Session | L15 state | Flash JEDEC | Reading |
|---|---|---|---|
| Step 2 (early bring-up) | nRESET no pull-up, no 9151 drive → floating, mostly held in reset by leakage | `EF 40 18` PASS | Flash worked because L15 wasn't actively driving the bus |
| Step 6.4 (10 kΩ pull-up soldered) | nRESET held HIGH → L15 boots and runs freely | (not measured immediately) | L15 starts contending |
| Step 7 onwards | L15 booting on every power cycle | `00 00 00` and similar | Flash regressed; we blamed BUCK2 voltage and regulator framework |
| Step 12.7 (today, `HOLD_L15_IN_RESET=1`) | L15 forcibly held LOW → GPIOs high-Z | `ff ff ff` | Bus uncontested but **no chip responds — MISO floats** |

**The insight: holding the L15 in reset removes the contention but the flash and IMU still don't respond.** That's not "bus broken" — that's "slaves' output drivers dead." Two-master contention on a shared MISO is exactly the failure mode that can damage SPI peripherals: when both the L15 and (say) the W25Q128JV try to drive MISO in opposite directions, current flows directly through their output stages with only their internal series resistance limiting it. Sustained over many boot cycles, this can either:

- Burn out the slave's MISO output transistor, leaving it permanently high-Z (consistent with `ff ff ff` floating), or
- Latch up the slave's output stage, leaving it permanently low (consistent with `00 00 00` even when the L15 is silent — though we never confirmed this case)

**This means step 12's "supply margin / dts / SPI mode" debugging chase was looking at the wrong layer.** The flash and IMU were already damaged by the time we started measuring. The voltage / mode iterations we did couldn't have recovered them; they were chasing software fixes for a hardware-destroyed peripheral.

**What this says about the design (v2 implications):**

1. **Sharing a SPI bus between two MCUs is a fundamental design hazard.** Even with a designated single master and the other "supposed to" be a slave, a runaway state on the second MCU at boot can cause exactly the contention we saw. The cost is silent slave damage on the bus.
2. **The previously-noted "give the L15 its own SPI bus" v2 delta (step 8 / 12.8) is now upgraded from "good practice" to "mandatory for any v2 silicon".** The damage potential is too real.
3. **A pre-flash safety pattern**: if a v2 must share a bus across MCUs (it shouldn't, but if it must), have one of them *physically gate* the other's SPI pins — e.g., a TXS-style level translator with an enable pin held LOW until coordination protocol is established. Otherwise the boot-race will eventually damage something.

**Implication for the remaining 2 untested boards:** they may have *intact* flash and IMU IF they've never been subjected to L15+9151 SPI contention. If we can validate them with the L15 firmly held in reset *from first power-on*, we might confirm the SPI3 path actually works on these units. Worth doing before declaring SPI3 broken at the design level.

### 12.7 — nRF54L15 was driving the SPI3 bus; confirmed by erase-and-isolate test

The 9151 isn't the only host on SPI3. The nRF54L15 also has its SPI master tied to the same SCK/MOSI/MISO physical lines (board design choice, presumably for inter-MCU coprocessor traffic). When the L15 boots — which it does freely once the 10 kΩ nRESET pull-up was soldered (step 6.4) — its GPIOs can drive those lines and contend with the 9151's SPI3 master.

**Diagnostic**: erased the L15's flash entirely (so no firmware running) AND held it in reset by driving 9151 P0.19 LOW (overriding the 10 kΩ pull-up). With both belts and braces, the L15's GPIOs are guaranteed high-Z. Re-ran the 9151 SPI flash probe:

```
flash JEDEC attempt 1/3: 00 7f ff (unexpected; will retry)
flash JEDEC attempt 2/3: ff ff ff (unexpected; will retry)
flash JEDEC attempt 3/3: ff ff ff (unexpected; will retry)
imu WHO_AM_I (0x0F) = 0xff
```

The pattern flipped from **driven** values (`00` board #2, `17` board #1) to **floating** values (`ff` settling from `00 7f ff` on attempt 1 → `ff ff ff` steady on later attempts). `FF` with the SPI master clocking is the classic signature of MISO floating to its weak internal pull-up — *no chip is responding*. The settle from `00 7f ff` to `ff ff ff` over the first read also fits: the bus needs a few transitions to stabilise after CS/SCK activity in a high-impedance bus state.

**Two independent issues on SPI3 now isolated:**

1. **L15 was contending with the bus when active.** Confirmed by the pattern change between L15-active and L15-erased-in-reset. Different boards produced different driven values because each board's L15 was in a slightly different (but spurious) GPIO state. Hardware-side fix on next board spin: don't share SPI master between two MCUs — give each its own bus, or wire the L15 strictly as a *slave* on the 9151's bus. Software-side mitigation in current code: the `HOLD_L15_IN_RESET 1` toggle in `main.c` drives P0.19 LOW to keep the L15 high-Z whenever the 9151 needs SPI3 to be uncontested.
2. **Flash and IMU still don't respond even with L15 out.** Floating MISO (`ff`) means CS goes out, SCK clocks, MOSI carries the command, but **no chip drives MISO during the response phase**. Either:
   a. The chips aren't powered (multimeter VCC at flash and IMU pads to confirm BUCK2 = 2.9 V is actually reaching them, not just the BUCK2 output)
   b. CS / SCK / MOSI aren't reaching the chips (continuity from SoC pads to chip pads)
   c. The chips themselves were damaged by earlier bus contention with the L15 (current spikes when both masters drive the same line in opposite directions)

### 12.8 — credit where due, and the next-session diagnostic plan

The key insight came from the user during the day's debug — "the nrf54l15 is also connected to the same spi line, maybe that's why it's not working." That's exactly what was happening. Two-master SPI is one of those things that's invisible from firmware alone (the SoC's master peripheral has no idea anyone else is on the bus) and hard to spot if you're focused on flash/IMU symptoms.

**Next-session plan, in priority order:**

1. **Confirm chip-side power** with a multimeter on the flash and IMU's actual VCC pins. If anything reads ≠ 2.9 V there's a broken trace between BUCK2 and that chip.
2. **Continuity-check CS / SCK / MOSI** from SoC pad to chip pad on both flash and IMU.
3. **Scope the SPI3 lines during a JEDEC read** (with `HOLD_L15_IN_RESET = 1` so the bus is uncontested):
   - SCK should be cleanly clocking
   - MOSI should show 0x9F on the first byte
   - CS should pulse LOW for the duration of the transaction
   - MISO should show the chip's response on bits 8..31 of the transaction. Currently floats high — this is the smoking-gun line to instrument.
4. If signals at the SoC pad look fine but at the chip pad MISO still floats → reflow the chip's solder joints or replace the chip.

**Schematic recommendation for next board spin** (now a hard requirement, not a nice-to-have):
- nRF7000 already gets a separate SPI bus from the flash/IMU (good, follows current direction).
- nRF54L15 should get **its own SPI bus** for any 9151↔L15 communication. Sharing master roles between two MCUs on one bus is asking for the kind of debugging hell we just spent half a day in.
- Document the bus topology in the schematic with explicit master/slave arrows so it's obvious at a glance who can drive what.


---

## Step 13 — nRF7000 Wi-Fi end-to-end win (9 APs, both bands)

### 13.1 — what worked

Final RTT after a long debug session:

```
[boot] wifi_nrf_bus: SPIM spi@9000: freq = 8 MHz
[boot] wifi_nrf: nrf_wifi_fmac_otp_mac_addr_get: MAC addr not programmed in OTP
[boot] wifi_nrf: random MAC generated (CONFIG_WIFI_RANDOM_MAC_ADDRESS=y)
[boot] wifi_nrf: firmware patch upload OK
[t=15s] wifi_probe: Wi-Fi interface ready (ifindex 1), waiting for it to come up...
[t=19s] wifi_probe:   [ 1] MIWIFI_4552                      ch=11  rssi= -56 dBm  4e:10:81:f7:45:54  WPA2-PSK
[t=19s] wifi_probe:   [ 2] (hidden)                         ch=36  rssi= -64 dBm  4a:10:81:f7:45:57  WPA-PSK
[t=19s] wifi_probe:   [ 3] MIWIFI_4552                      ch=36  rssi= -65 dBm  4a:10:81:f7:45:53  WPA2-PSK
[t=19s] wifi_probe:   [ 4] W_JLG                            ch=4   rssi= -76 dBm  74:4d:28:b9:c0:52  WPA2-PSK
[t=19s] wifi_probe:   [ 5] Livebox6-9BB0                    ch=6   rssi= -77 dBm  50:6f:0c:4c:9b:b4  WPA2-PSK
[t=19s] wifi_probe:   [ 6] ARLO_VMB_7396778195              ch=1   rssi= -81 dBm  fc:9c:98:a6:6d:c8  WPA2-PSK
[t=19s] wifi_probe:   [ 7] MOVISTAR-WIFI6-72B0              ch=1   rssi= -87 dBm  90:d3:cf:b6:72:bf  WPA2-PSK
[t=19s] wifi_probe:   [ 8] Livebox6-83BF                    ch=6   rssi= -89 dBm  84:90:0a:06:83:be  WPA2-PSK
[t=19s] wifi_probe:   [ 9] R&A                              ch=6   rssi= -90 dBm  28:d1:27:4c:73:b2  WPA-PSK
```

That's:

- **9 APs** across 2.4 GHz (channels 1, 4, 6, 11) and 5 GHz (channel 36)
- RSSI from **-56 dBm** (strong, MIWIFI_4552 — likely the user's home network) down to **-90 dBm** (marginal but decoded)
- Hidden SSIDs detected (no SSID broadcast, but BSSID + security still readable)
- Security modes decoded: WPA-PSK, WPA2-PSK
- Total scan time: ~4 s after netif came up

The whole chain is validated end-to-end:
- nRF7000 power-up sequence (BUCKEN → IOVDD-CTL via TCK106AG load switches)
- SPI handshake at 8 MHz
- OTP MAC read attempt (blank on this dev part — handled by random-MAC fallback)
- Firmware patch upload from Nordic blob into RPU
- Netif registration through `nordic,wlan` child node
- Driver init completion (admin-up after ~4 s)
- `net_mgmt(WIFI_SCAN, ...)` triggering passive scan on both 2.4 and 5 GHz
- Per-AP `NET_EVENT_WIFI_SCAN_RESULT` events flowing through to our callback
- Final `NET_EVENT_WIFI_SCAN_DONE` releasing the semaphore
- Antenna routing via the BGS12SN6E6 RF switch — turns out to default into a usable position even with SW_CTRL0 high-Z (good news, was a question mark earlier)

### 13.2 — the configuration journey (so the next bring-up doesn't take a day)

The thing that almost everyone gets wrong on this is **NCS 3.3.0 forces nRF70 enablement from sysbuild, not application Kconfig**. We hit every variation:

| What we tried | Result | Lesson |
|---|---|---|
| `CONFIG_WIFI_NRF70=y` in `prj.conf` only | Driver not compiled, `no Wi-Fi interface` | Sysbuild overrides it back to `=n` silently |
| Set `CONFIG_NRF70_SCAN_ONLY=y` in `prj.conf` only | Kconfig warning "selected but no symbol ended up as choice selection" + driver not registered | The choice's parent winner has to be picked at sysbuild level too |
| `SB_CONFIG_WIFI_NRF70=y` + leave `prj.conf` alone | Build progresses further — driver gets compiled — but cmake target collision (`gen_nrf70_bin_inc cannot create target`) | Both sysbuild and the now-stale app-side `CONFIG_NRF_WIFI_PATCHES_*` lines were registering the patch-blob handler. Pick *one* — sysbuild |
| Sysbuild only + clean `prj.conf` + missing `west blobs fetch nrf_wifi` | `Blob for path .../nrf70.bin missing` | Patches are binary blobs not in the source tree; must be fetched once per NCS install |
| All of the above + missing `wlan0` child node | `'DT_N_INST_0_nordic_wlan_FULL_NAME' undeclared` | The driver's instance macros expect a `compatible = "nordic,wlan"` netif child under the nrf70 SPI node |
| All of the above + RAM | Region overflow by 37 KB | nRF70 driver needs significant RAM; on 9151+TF-M-NS we landed at `CONFIG_HEAP_MEM_POOL_SIZE=57344` (56 KB) which fits |
| All of the above + scan-too-soon | `net_mgmt(SCAN) failed: -115 (-EINPROGRESS)` | Driver init is async; netif comes up a moment after `net_if_get_first_wifi()` returns. Poll `net_if_is_admin_up()` for a few seconds before requesting the scan |
| All of the above + OTP MAC blank | `MAC addr not programmed in OTP` → `nrf_wifi_if_start_zep: Failed to get MAC` | Production parts come MAC-provisioned; dev parts often don't. `CONFIG_WIFI_RANDOM_MAC_ADDRESS=y` makes the driver invent a locally-administered MAC at startup |

Final, working setup (canonical version, save this for future bring-ups):

**`sysbuild.conf`** (single source of truth for the operating-mode + patch-handling Kconfigs):
```
SB_CONFIG_BOOTLOADER_MCUBOOT=n          # bring-up: no MCUboot, app at 0x0
SB_CONFIG_WIFI_NRF70=y                  # enable nrf70 driver
SB_CONFIG_WIFI_NRF70_SCAN_ONLY=y        # scan-only mode (smaller, no STA/AP)
SB_CONFIG_WIFI_PATCHES_EXT_FLASH_DISABLED=y  # patches built into image, no ext flash
```

**`prj.conf`** (only generic networking + Wi-Fi mgmt + heap; NO `CONFIG_WIFI_NRF70` etc.):
```
CONFIG_NETWORKING=y
CONFIG_NET_L2_WIFI_MGMT=y
CONFIG_NET_MGMT=y
CONFIG_NET_MGMT_EVENT=y
CONFIG_NET_MGMT_EVENT_INFO=y
CONFIG_WIFI=y
CONFIG_HEAP_MEM_POOL_SIZE=57344
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096
CONFIG_WIFI_RANDOM_MAC_ADDRESS=y
```

**`board common dtsi`** — nrf70 child of spi1 with all 9 required tx-power props + the `wlan0` netif child:
```
&spi1 {
    nrf70: nrf7000@0 {
        compatible = "nordic,nrf7000-spi";
        ...
        wifi-max-tx-pwr-2g-dsss      = <21>;
        wifi-max-tx-pwr-2g-mcs0      = <16>;
        wifi-max-tx-pwr-2g-mcs7      = <16>;
        wifi-max-tx-pwr-5g-low-mcs0  = <13>;
        wifi-max-tx-pwr-5g-low-mcs7  = <13>;
        wifi-max-tx-pwr-5g-mid-mcs0  = <13>;
        wifi-max-tx-pwr-5g-mid-mcs7  = <13>;
        wifi-max-tx-pwr-5g-high-mcs0 = <12>;
        wifi-max-tx-pwr-5g-high-mcs7 = <12>;
        wlan0: wlan0 { compatible = "nordic,wlan"; status = "okay"; };
    };
};
```

**One-time setup**: `west blobs fetch nrf_wifi` to download the firmware patch from Nordic.

### 13.3 — the design protections that paid off

After board #1 was hurt during an earlier debug session, we put two protections in `wifi_probe.c` before bringing up the nRF7000 again:

1. **`k_sys_fatal_error_handler`** that drives BUCKEN + IOVDDCTL LOW immediately on any fatal Zephyr error (hard fault, k_oops, watchdog, stack overflow). Cuts nRF7000 power *before* the fault dump prints — by the time the system halts, the chip is unpowered.
2. **`wifi_emergency_off()`** exposed for explicit calls. Idempotent and ISR-safe.

Neither got triggered this session — clean run all the way through — but the safety net was there. **Strongly recommend keeping this pattern for any high-current peripheral that's host-MCU-gated.** If main() crashes for any reason, the worst-case is "chip stays unpowered until next reset," not "chip burns through current at full TX while the host is dead."

### 13.4 — what's left

**Wi-Fi side: bring-up is genuinely complete.** We can scan, get rich AP data, and the driver+chip+antenna+RF-switch path is all working. Production-grade work from here is:

- **A-GNSS / Wi-Fi positioning**: feed scan results to nRF Cloud's Wi-Fi locator API for cellular-free location fixes. Same `nrf_cloud_*` library as the GNSS A-GNSS path from step 11. Multi-hour integration but a solid product feature.
- **OTP MAC provisioning**: production parts will come MAC-programmed; the random-MAC fallback should be removed for production builds (or kept as defensive).
- **Active scan ban remains in force**: passive only because of the BUCK current limit (step 9 / `project_palroc_nrf7000_supply.md`). The scan we just did was passive (set explicitly in `wifi_scan_params.scan_type`); confirmed working under that constraint.

**Other parked items unchanged from earlier steps:**
- SPI3 flash + IMU still failing (board-level wiring, see step 12)
- L15 BLE on custom board (nRESET pull-up + HFXO load cap from earlier sessions)
- GNSS production-grade fix needs A-GNSS (step 11)

But the major outstanding question of "can we even talk to the nRF7000 on this board" is now decisively answered: **yes, including 5 GHz scan**. That was the day's win.


---

## Step 14 — DANGER: USB-disturbing fault when L15 SWD is connected; 4.2 V on BUCK2; suspect MPPT-input + hall1 fault

### 14.1 — symptom

Connecting the J-Link SWD probe to the **9151 side** of the board: works fine, RTT comes up cleanly, no host-side disturbance.

Connecting the J-Link SWD probe to the **nRF54L15 side** while the board is **powered via PPK2**: the host laptop misbehaves catastrophically:

- Bluetooth disconnects (mouse, headphones drop)
- The USB hub the J-Link is plugged into drops off
- Keyboard and other USB peripherals stop responding
- Recovery requires unplugging everything and re-enumerating

This pattern **does not occur** when the board is powered from a battery (instead of through PPK2 + USB hub). That's a strong tell that **a current transient is being dragged through the J-Link's USB path back into the laptop's USB hub**. USB host hubs typically brown out at ~500 mA per downstream port; whatever the board is doing pulls more than that, momentarily, through the J-Link's USB cable.

### 14.2 — concurrent fault: BUCK2 reading 4.2 V (was 2.9 V earlier in the session)

Independent multimeter measurement on the same boards: **the 3V3 rail (BUCK2 output) reads 4.2 V**, where it was correctly 2.9 V earlier in this same session.

4.2 V is exactly the voltage of a **fully-charged Li-Ion VBAT/VSYS** on the npm1300. So either:

- A short between VSYS and BUCK2 OUT on the PCB has appeared (solder bridge, damaged trace, damaged pin)
- The npm1300's BUCK2 has failed in pass-through and is dumping VSYS to its output
- The chip has been put into a bad state (latch-up?) by something we did

4.2 V is **above the absolute max VDDIO** for every chip on that rail:

| Chip | Max VDDIO | At 4.2 V |
|---|---|---|
| W25Q128JV NOR flash | 3.6 V | over by 0.6 V |
| LSM6DSO IMU | 3.6 V | over by 0.6 V |
| nRF9151 VDDIO_GPIO | 3.6 V | over by 0.6 V (SoC stress) |
| nRF7000 VDDIO | 3.6 V | over by 0.6 V |
| nRF54L15 VDDIO | 3.6 V | over by 0.6 V |

So every chip on the 3V3 rail is being silicon-stressed under this condition. Extended exposure damages them; brief exposure may be survivable but cumulative.

### 14.3 — hypothesis: hall1 + MPPT-input + npm1300 latch chain

The user's hypothesis is that this might be a **chain reaction starting from two known board faults**:

1. **hall1 (P0.00) is broken** — see step 12.7 / button_probe.c FIXME. Hall sensors don't normally cause power issues, but a damaged hall IC can short its supply rail or draw uncontrolled current. If hall1 is on the same rail as the rest of the 3V3 circuitry and is faulting at boot, that's a possible source of the inrush.
2. **MPPT input on the npm1300 has not been validated** and **no PV panel is connected**. The npm1300 has an integrated MPPT charger for solar-input designs; if it's been left enabled in OTP / dts but the VPV pin is floating, the chip can:
   - Oscillate trying to find a maximum power point that doesn't exist
   - Misbehave on its internal control loops
   - Possibly cause the BUCK2 voltage to drift / over-shoot (the 4.2 V symptom)
   - Drive transients on the system rails that propagate into anything else connected

The combined picture: at boot, hall1's fault + MPPT's floating input pushes the npm1300 into a bad regulation state, BUCK2 spikes to ~4.2 V (passing VSYS), the inrush of charging the board's bulk caps via the J-Link's USB-side ground path browns out the host's USB hub. Connecting via the 9151 SWD doesn't trigger this, presumably because the 9151's SWD ground and signal returns are routed differently than the L15's.

This is **a hypothesis, not a confirmed diagnosis**. It's consistent with the symptoms but we haven't proven causation. To prove it, the next debug session should:

1. **Disconnect the hall1 IC** (lift one pin or depopulate) before re-validating
2. **Verify the MPPT input is correctly disabled in npm1300 configuration**, or short the VPV pin to ground via a resistor so it's at a known state
3. **Re-measure BUCK2 with the chip booted** — should be back at 2.9 V
4. **Reconnect the L15 SWD** — see if the USB-disturbing transient goes away

### 14.4 — boards-burned count and decision to halt

The user has so far tested **3 boards out of 5**. All three may have been exposed to:
- BUCK2 = 4.2 V transients (over-spec on every connected chip)
- npm1300 bad-state behaviour with the MPPT floating
- The hall1 fault

Some of those chips may now be partially damaged in ways we haven't characterised. Symptoms of cumulative damage on chips run beyond max VDDIO are often subtle: slightly elevated leakage current, occasional bit errors, reduced longevity — all things that don't show up in a quick functional test.

**Decision: stop board-by-board testing of the remaining 2 boards until the root cause is understood.** Specifically:

- Do not connect the J-Link to the L15 SWD on a board that has hall1 populated and an MPPT-disabled-but-not-grounded VPV input.
- Pre-board-test rework checklist:
  - Disconnect / depopulate hall1
  - Verify VPV pin state on the npm1300 (ground if MPPT not in use)
  - Power up via battery only (not PPK2 USB) for first verification
  - Multimeter on BUCK2 BEFORE any chip-side activity — must read 2.9 V
- Only after the above checks pass, re-add USB-side power and proceed with full bring-up.

### 14.5 — what to investigate / fix before re-engaging

The unknowns that need answering before resuming board validation:

1. **Is the npm1300 MPPT enabled?** Check the dts charger node's `pwr-gpios`, `vbus-limit`, and any `nordic,npm1300-charger` properties relating to PV input. We currently have:
   ```
   term-microvolt = <4150000>;
   current-microamp = <150000>;
   dischg-limit-microamp = <1000000>;
   vbus-limit-microamp = <500000>;
   thermistor-ohms = <10000>;
   thermistor-beta = <3380>;
   /* charging-enable; — left off */
   ```
   No explicit MPPT property. Check if the chip's OTP / factory default has MPPT enabled.

2. **Is hall1's fault electrically isolatable?** What rail does it share with what? Is there an inrush path from VSYS through hall1 to GND when the chip glitches?

3. **What's the actual schematic relationship** between BUCK2 OUT, VSYS, and the npm1300's pin pads? Is there a short anywhere visible under magnification?

4. **What does PPK2 see** on the supply during the moment the L15 SWD is connected? A PPK2 trace at high sample rate might catch the inrush event and tell us how much current spikes for how long.

### 14.6 — consequences for next board spin

Already-known v2 hardware deltas (steps 8 / 9 / 11 / 12), now expanded:

1. Move LSM6DSO from SPI3 to i²c2
2. Move L15 SPI to its own bus (not shared with the flash + IMU)
3. SAW filter before the GNSS LNA
4. R44 / VBAT supply path rework
5. **NEW: explicitly tie the npm1300 VPV pin to ground (or to whatever the production design specifies for "no PV panel")** so the chip cannot enter MPPT-search mode against a floating input
6. **NEW: investigate hall1's footprint / supply path** — if it's been causing this kind of fault on multiple boards, it's not an assembly defect on one unit, it's a design problem
7. **NEW: assess BUCK2 routing** for shorts to VSYS — if the issue is reproducible across boards, it's likely a layout proximity / unintended-coupling problem
8. **NEW: add a TVS diode on the 3V3 rail (and ideally on each chip-side power pin worth protecting)**. v1 omitted this — the TVS is *not* the cause of the 4.2 V over-voltage on BUCK2, but it would have clamped the transient at ~3.6 V and likely prevented the cumulative chip stress we've now incurred across 3 boards. Belt-and-braces protection that earns its cost the first time anything on the regulator side goes wrong. Standard candidates: PESD3V3L4UG (4-line array, ~3.3 V working voltage) for grouped IO/power nets, or an ESD9X3.3 (single-line, 3.3 V working voltage, very low capacitance) per critical net. Place close to the connector / regulator output it protects, with the shortest possible ground return.

### 14.7 — final note

This step is more "lessons captured during a near-miss" than "bring-up complete." The session today proved nRF7000 Wi-Fi works end-to-end (step 13), but discovered a serious fault chain on the 9151+npm1300 side that was previously masked by the SPI3 contention and the L15-in-reset state. **Two boards are now considered "do not test until validated rework"** — that's a substantial finding worth more than any individual debug win.


---

## Step 15 — L15 BLE end-to-end + RF-switch-doesn't-matter discovery

### 15.1 — the LFXO-Kconfig fix that unlocked everything

After many hours stuck on "L15 doesn't produce RTT output, chip seems alive on SWD but firmware never runs," the final fix was a 2-line Kconfig pair:

```
CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=n
```

The palroc board doesn't have a 32.768 kHz LFXO crystal on XL1/XL2 — it relies on the internal 32 kHz LFRC oscillator. The dts trick of `&lfxo { status = "disabled"; }` is *necessary but not sufficient*: it tells the chip-side LFXO peripheral to stay off, but Zephyr's clock-control subsystem can still wait for LFXO startup at boot depending on which init path it takes. Without the K32SRC Kconfig pair, the kernel hangs waiting for a clock that will never come up.

Symptoms when this is missing on a board with no LFXO:

- Chip is provably alive on SWD (`JLinkExe halt + regs` shows valid PC, SP, etc.)
- But no RTT output ever appears
- Looks identical to "RTT viewer can't find control block" or "chip is bricked" — but it's actually "kernel is alive but waiting for LFXO startup."

The earlier guess that auto-detection was failing because of TrustZone secure-only SRAM (saved as `feedback_nrf54l_rtt_address.md`) turned out to be a red herring: the chip just wasn't running far enough to set up the RTT control block in the first place. Once the K32SRC Kconfig is set, RTT auto-detect works fine.

Saved as `feedback_nrf54l_lfxo_missing.md` so future bring-ups don't have to re-discover this.

### 15.2 — BLE radio validated end-to-end

After the K32SRC fix, the L15 boots and the BLE-broadcaster sample (already in `nrf54l15/src/main.c`) starts advertising. Verified from a phone running nRF Connect for Mobile:

| Test | Result |
|---|---|
| L15 boots cleanly | ✓ |
| RTT auto-detect works | ✓ (no manual control-block lookup needed) |
| BLE controller initialises | ✓ |
| Advertising visible as `palroc-l15` | ✓ |
| Close-range RSSI (phone within ~30 cm) | **-57 dBm** (excellent — strong-signal territory) |
| Through-walls range (10–15 m, ≥1 wall between) | **-95 dBm** (at BLE 1M-PHY sensitivity edge, but still detectable) |

Path-loss math sanity check:
- Free-space 2.4 GHz over 10 m: ~60 dB
- Add ~10–20 dB through interior walls / furniture
- Phone antenna at 0 dBi, body-shadowed
- Expected RSSI delta: ~70–80 dB → observed delta from close-range was ~38 dB, well within margin

That's typical PCB-antenna performance at 2.4 GHz. Nothing wrong with the radio path. For longer-range future work, **BLE LE Coded PHY** would buy ~10-12 dB of additional link budget — Kconfig flip on the controller side, no hardware change needed.

### 15.3 — the surprise: RF switch doesn't matter for bring-up

Today's surprise data point: the BGS12SN6E6 SPDT antenna switch is currently **unpowered** (LDO2 didn't make it to the part on this board, or the rail is not actually connected) AND its enable signal SW_CTRL0 isn't driven (the L15 GPIO isn't configured to drive it yet, and the gate-driver MOSFET RV2C010UNT2L is itself questionable).

Despite all of this, the radios work:

- **Wi-Fi scan via nRF7000** (step 13): 4 APs visible at -67 to -85 dBm even with the switch entirely out of the loop
- **BLE advertising via nRF54L15** (step 15.2): -57 dBm close-range, -95 dBm through walls

How this works:

1. **The BGS12SN6E6 has finite isolation, not infinite.** SP2T switches at 2.4 GHz typically have 25-40 dB of isolation between paths. ~10⁻³ to 10⁻⁴ of the signal still leaks through the "off" path. For strong nearby APs / phones, that leakage is plenty to register and even to negotiate connections.
2. **The default position with VC undriven is path-dependent.** Many SP2T switches have an internal pull on VC that defaults to one of the two paths. If that default happens to favour either Wi-Fi or BLE, the corresponding chip just gets the antenna by default with no active switching needed.
3. **At 2.4 GHz, even PCB trace and package coupling can deliver usable RF power** to the chip from the antenna feed. Strong nearby radios couple in capacitively across mm distances.

Quantified comparison (Wi-Fi scan): switch operating (step 13) vs switch unpowered (step 15):

| Metric | Switch in path (step 13) | Switch out of path (today) |
|---|---|---|
| Best RSSI | -56 dBm | -67 dBm |
| AP count | 9 | 4 |
| Bands seen | 2.4 + 5 GHz | 2.4 + 5 GHz |

So **the switch is buying ~10 dB of received-signal margin** — meaningful for distance and weak APs, not catastrophic when missing.

**Implications:**

- For *bring-up* purposes — i.e. "does this chip work at all" — the RF switch is not on the critical path. You can validate Wi-Fi and BLE without it.
- For *production-grade RF performance* — full sensitivity, distant APs, weak-signal stations, longest BLE range — the switch needs to be powered and correctly switched. The 10 dB matters.
- **The previously-noted concern about SW_CTRL0 routing through the L15 (which would mean Wi-Fi only works when L15 is active) is moot for bring-up.** It still matters for production: when both radios run concurrently, switch coordination becomes essential.

### 15.4 — current platform validation status

After today's session, the validated capabilities of the platform:

| Subsystem | State | Notes |
|---|---|---|
| nRF9151 modem | ✓ end-to-end | LTE attach NB-IoT B20 / B8, Orange ES roaming on board #2 |
| GNSS RX path | ✓ | LDO1↔COEX0 level-shift fix, 12 SVs tracked, best CN0 41 dB-Hz outdoors |
| nPM1300 telemetry | ✓ | VBAT, NTC, IBAT all readable; charging at 158 mA validated |
| Battery charging | ✓ | matches `current-microamp` setting |
| Supply rails | ✓ | BUCK1=3.3V, BUCK2=2.9V, LDO1=2.9V, LDO2=2.9V (multimeter-verified) |
| I²C bus + scan | ✓ | 0x6b PMIC; 0x0b confirmed phantom |
| nRF7000 Wi-Fi scan | ✓ | Both 2.4 + 5 GHz, 9 APs in step 13, 4 APs even with switch off |
| Hall sensor (hall2) | ✓ | active-LOW open-drain on P0.01 |
| 3 LEDs | ✓ | After MOSFET package rework |
| RTT logging | ✓ | both 9151 + L15 |
| nRF7000 emergency-off | ✓ | fatal-error handler protects against runaway firmware |
| **nRF54L15 BLE advertising** | ✓ | **today's win — -57 dBm close, -95 dBm at 15 m** |
| **nRF54L15 boot + RTT** | ✓ | **today's win — K32SRC Kconfig was the fix** |

So **all three radios** (LTE, GNSS, BLE, Wi-Fi) are now validated. Of the three Nordic chips on the platform, **the chip-side bring-up is essentially complete.**

### 15.5 — what's still parked / open

| Item | Why still parked | Path forward |
|---|---|---|
| SPI3 flash + IMU corrupt reads | Section 12.6.5 hypothesis: chips electrically damaged by L15+9151 SPI bus contention | Move LSM6DSO to i²c2 in v2; give L15 its own SPI bus in v2 |
| hall1 sensor (P0.00) broken | Likely board-level wiring / package issue | Investigate footprint on next spin; possible swap to different sensor |
| BUCK2 = 4.2 V incident (step 14) | npm1300 misregulating; suspected MPPT-input + hall1 fault chain | Disconnect hall1 + ground VPV, re-test |
| MPPT validation | Wrong passive values set charging voltage too low; MPPT never triggered | **Resolved (2026-05-05)** — corrected passive values, MPPT now triggers and works correctly |
| Laptop USB hub crashes when L15 SWD plugged in | Inrush-current pulled through J-Link's USB return | USB isolator between J-Link and laptop; v2 supply rework |
| GNSS production-grade fix | Marginal CN0 (~28 dB-Hz typical), insufficient for ephemeris decode | A-GNSS via nRF Cloud (step 11 roadmap) |
| RF switch full validation | Antenna coupling is good enough to fake working radios | Power LDO2 → switch VDD; drive SW_CTRL0 from a chip; verify the +10 dB sensitivity bump |

**But none of those block the basic claim that the chips work and the platform is functional at the radio level.** v2 hardware deltas are well-defined (8 items in step 14.6).

### 15.6 — closing note

Today and this week have been the productive end of a multi-week bring-up. Genuine wins captured: LTE, GNSS, BLE, Wi-Fi, charging, telemetry, hall, and the supporting infrastructure (RTT, fatal-error protection, sysbuild config, voltage rails). Lessons saved as memories so future bring-ups don't relive any of this.

The platform is now ready for application development on top of the radios. The hardware quirks are all known, characterised, documented, and have v2 fixes already specified. Strong place to be.


---

## Step 16 — i²c2 contention proven (same as SPI3); bring-up handoff to application firmware

### 16.1 — i²c2 contention finding (mirrors the SPI3 story exactly)

After step 15's L15 BLE win, re-running the 9151 bring-up with the L15 *active* (no longer held in reset) showed the same kind of degradation we'd seen on SPI3 — but on **i²c2** this time:

```
[00:01:06.580,505] <wrn> i2c_probe:   no devices responded
[00:01:06.680,603] <inf> main: --- nPM1300 read (VBAT / NTC / IBAT) ---
[00:01:06.680,603] <err> npm1300: npm1300 charger device not ready (driver bound? dts wired?)
```

Then **erasing the L15** (so its firmware doesn't run, GPIOs default to high-Z) and re-flashing the 9151 alone:

```
[00:00:15.004,425] <inf> npm1300: nPM1300 charger sample:
[00:00:15.004,455] <inf> npm1300:   VBAT   : 3.867000 V
[00:00:15.004,547] <inf> npm1300:   Tntc   : 22.856903 C
[00:00:15.004,577] <inf> npm1300:   IBAT   : 0.000000 A
```

Clean recovery. So the L15 was contending on the i²c2 bus too. The L15's i²c-master pins are wired to the *same physical net* as the 9151's i²c2 (L15 P1.04 ↔ SDA ↔ 9151 P0.09; L15 P1.05 ↔ SCL ↔ 9151 P0.08). With the L15 active and its GPIOs not parked, the bus was being held in some state that broke 9151-side master traffic.

Side-by-side with the SPI3 contention story (step 12.7):

| Bus | L15 active | L15 erased / held LOW |
|---|---|---|
| SPI3 (W25Q128JV + LSM6DSO + L15 SPI) | corrupt-but-deterministic reads (`00`/`17`); chips eventually damaged | floating MISO, but slaves now also dead (12.6.5) |
| i²c2 (nPM1300 + L15 i²c) | bus silent, no ACKs from anyone | 0x6b PMIC ACKs, telemetry reads cleanly |

**i²c2 is in better shape than SPI3** because i²c is open-drain — contention doesn't burn output drivers the way SPI's push-pull contention does. So the npm1300 has survived the contention episodes that destroyed the SPI3 slaves. The bus comes back to life cleanly when the L15 is silent.

### 16.2 — defensive workaround in L15 firmware

Added a `SYS_INIT(POST_KERNEL, 50)` hook in `nrf54l15/src/main.c` that explicitly parks **L15 P1.04 and P1.05 as high-Z `GPIO_INPUT | GPIO_DISCONNECTED`** at boot, before anything else runs. This is a defensive measure: even if the application firmware misconfigures these pins later, at least at boot they're guaranteed not to interfere with the 9151's i²c2 bus.

```c
static int l15_park_shared_i2c(void)
{
    const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (!device_is_ready(gpio1)) return -ENODEV;
    gpio_pin_configure(gpio1, 4 /* SDA */, GPIO_INPUT | GPIO_DISCONNECTED);
    gpio_pin_configure(gpio1, 5 /* SCL */, GPIO_INPUT | GPIO_DISCONNECTED);
    return 0;
}
SYS_INIT(l15_park_shared_i2c, POST_KERNEL, 50);
```

This is **not** the long-term answer. The long-term answer is one of:
- Move the L15 off the shared i²c2 bus entirely (v2 hardware delta)
- Configure the L15 as an explicit i²c slave on the 9151's bus (a designed inter-MCU communication topology)
- Multi-master i²c with arbitration (rarely worth the complexity)

### 16.3 — full v2 hardware deltas, consolidated

Updating the v2 list from steps 8 / 9 / 11 / 12 / 14, now adding the i²c2 finding:

1. Move LSM6DSO from SPI3 to i²c2 (still valid — but...)
2. **NEW: L15's i²c2 sharing must also be resolved.** Either give the L15 its own i²c bus (preferred) or commit to a designed multi-MCU topology with the L15 as slave. Sharing-as-equals is what we've been doing and it doesn't work.
3. Move L15 SPI to its own bus (already noted)
4. SAW filter before the GNSS LNA
5. R44 / VBAT supply path rework
6. Tie npm1300 VPV pin to ground when no PV panel (or whatever production design specifies)
7. Investigate hall1 footprint / supply path
8. Assess BUCK2 routing for shorts to VSYS
9. Add TVS diode on 3V3 rail and ideally per-chip-power-pin (PESD3V3L4UG or ESD9X3.3)

Combined v1 lessons → v2 hardware spec is now reasonably complete: **9 well-characterised deltas**, all evidence-driven from this v1 bring-up.

### 16.4 — bring-up phase: complete; handoff to application firmware

Today's i²c2 finding closes the last "is the hardware path actually functional" question. From here, the work pattern shifts:

**This was bring-up engineering** (what you've been doing for weeks):
- Validate each chip is alive and communicates over its bus
- Characterise voltage rails, signal levels, RF paths
- Discover and document hardware quirks (level shifting, missing pull-ups, OTP defaults, MPPT behaviour, package solder issues, etc.)
- Define v2 hardware deltas

**Application firmware** (hereafter, somebody else's job):
- Inter-MCU coordination (who owns the shared i²c2/SPI3 bus, when, how)
- Application-layer protocols on top of the validated radios
- Power management strategies (sleep modes, peripheral gating)
- Production firmware for end-user functionality (data collection, cloud upload, OTA updates, etc.)

The bring-up artefacts that hand off cleanly:

- **`steps.md`**: 1500+ lines of fully-attributed bring-up history. Anyone joining the project can read it cold and understand why every Kconfig, every dts choice, every rework decision is what it is.
- **Memory base**: 14 reference / project / feedback memories capturing reusable lessons (npm13xx driver paths, sysbuild-vs-prj.conf for nRF70, LFXO Kconfig pair, voltage step quantisation, regulator-always-on flag, RTT control block lookup procedure, MAC randomisation, etc.).
- **Working firmware in `src/`**: per-peripheral probes that demonstrate each chip works individually. Application engineer can use these as reference or starting templates.
- **Working L15 BLE-broadcaster app** in `nrf54l15/`. RTT, BLE, antenna all validated.
- **Captures folder**: PPK2 traces and scope shots from the harder debugging sessions.
- **Documented v1 hazards**: 9 v2 hardware deltas, the chip-damage history of SPI3, the BUCK2-overvolt incident, the USB-disturbing-fault, the floating-MPPT and hall1 mystery.

### 16.5 — final platform validation table

| Subsystem | State | Notes |
|---|---|---|
| nRF9151 modem (LTE attach) | ✓ | NB-IoT B20, Orange ES roaming, 76 s on board #2 |
| GNSS RX path | ✓ | 12 SVs / 41 dB-Hz before VBUS incident; LNA replacement sourced and in hand (2026-05-05); re-solder + re-verify before GNSS validation run |
| nPM1300 telemetry (VBAT/NTC/IBAT) | ✓ | Working, working, working (today re-confirmed) |
| Battery charging | ✓ | 158 mA matched to 150 mA setting |
| Supply rails (BUCK1/2/LDO1/2) | ✓ | All multimeter-verified at correct values; LDO1/2 now in regulated LDO mode (not load-switch) |
| i²c2 bus | ✓ | Working when L15 is silent; contended when L15 is active (16.1) |
| nRF7000 Wi-Fi scan | ✓ | 9 APs at full path; 4 APs even with switch off |
| nRF54L15 BLE advertising | ✓ | -57 dBm close, -95 dBm at 15 m through walls |
| nRF54L15 boot + RTT | ✓ | After K32SRC Kconfig + nRESET pull-up |
| Hall sensor (hall2) | ✓ | active-LOW open-drain on P0.01 |
| 3 LEDs | ✓ | After MOSFET package rework |
| Fatal-error chip protection | ✓ | nRF7000 power cut on any Zephyr fatal |
| MPPT (solar charger input) | ✓ | Wrong passives corrected; MPPT now triggers correctly (2026-05-05) |

**Parked (won't be revisited in bring-up phase)**:
- SPI3 flash + IMU (chips electrically damaged by L15+9151 contention; replace at v2)
- hall1 sensor (broken on multiple boards; investigate at v2)
- BUCK2 = 4.2 V incident (root cause confirmed: LDOIN = VSYS, LDOs wired as load switches → output = VSYS = 5 V on VBUS → 5 V leaked to 3.3 V rail. Firmware voltage settings for LDO1/LDO2 are ignored by hardware. v2 fix: connect LDOIN to regulated 3.3 V BUCK output, or rewire as proper LDO with feedback resistors)
- USB-hub-crashing inrush (fix via v2 supply rework + USB isolator for any remaining v1 work)

### 16.5c — LDO mode confirmed working; root cause of VSYS passthrough (2026-05-04)

Switched LDO1 and LDO2 from load-switch mode to LDO regulator mode via DTS:
- Added `#include <zephyr/dt-bindings/regulator/npm13xx.h>`
- Added `regulator-initial-mode = <NPM13XX_LDSW_MODE_LDO>;` to both LDO nodes
- Added `regulator-allowed-modes = <NPM13XX_LDSW_MODE_LDO>;` to match Nordic reference boards

After fix: LDO1 and LDO2 output the configured 2.9 V instead of VSYS.

**Root cause of why this wasn't working before:** The nRF54L15 was contending on i²c2 during the 9151 boot sequence. The `mfd_npm13xx_reg_write` calls that set LDSW[n]LDOSEL (LDO mode register) were failing or getting corrupted by L15 bus activity, so the PMIC driver silently left both channels in load-switch mode (hardware default = VSYS passthrough). Once the L15 firmware's `SYS_INIT(l15_park_shared_i2c, POST_KERNEL, 50)` parks P1.04/P1.05 as high-Z inputs before anything else runs, the i²c2 bus is clean and the PMIC writes succeed.

This is another concrete example of the L15 i²c2 contention causing silent failures — now confirmed to affect not just sensor reads but PMIC register writes during driver init.

### 16.5b — BUCK2 voltage tolerance confirmed (2026-05-04)

Tested both BUCK2 = 2.9 V and BUCK2 = 3.3 V (LDO1/LDO2 matching in each case). All downstream loads (W25Q128JV flash, LSM6DSO, nRF7000 VDDIO, RF switch, GNSS LNA path) operate correctly at either voltage. The nRF7000 VBAT ≥ VDDIO constraint is satisfied in both cases since BUCK1 = 3.3 V ≥ BUCK2.

Keeping BUCK2 = 2.9 V for v1 (leaves a 0.4 V VBAT–VDDIO margin). For v2, once LDOIN is tied to the regulated 3.3 V rail, standardising all rails at 3.3 V is a viable and simpler choice.

### 16.7 — SHPHLD + Hall sensor design clarification (2026-05-05)

**Sensor: DRV5032FBDBZR** (Texas Instruments, SOT-23-3, omnipolar, push-pull, 1.65–5.5 V, ~1.8 µA quiescent). Validated on bench — confirmed working.

**Measured output behaviour (bench-verified):**
- No magnet → output **HIGH**
- Magnet present → output **LOW**

**SHPHLD pin behaviour (nPM1300 datasheet):**
- Enter ship mode: I²C write to `TASKENTERSHIPMODE` register.
- Exit ship mode: SHPHLD pulled LOW (PMIC detects this and powers up).

**Compatibility table:**

| Condition | DRV5032 output | SHPHLD level | Result |
|---|---|---|---|
| No magnet | HIGH | HIGH | Ship mode stays latched ✓ |
| Magnet placed near device | LOW | LOW | PMIC exits ship mode, device powers on ✓ |
| Device running normally, no magnet | HIGH | HIGH | No effect — ship mode only re-entered via I²C ✓ |

**Use case (ship-mode activation):** Device is packed in ship mode (TASKENTERSHIPMODE set before packing). To activate, the end user holds a magnet near the PCB → DRV5032 output goes LOW → SHPHLD goes LOW → PMIC exits ship mode and powers up the system. Remove the magnet → output returns HIGH → system stays on.

**Power in ship mode:** All BUCK/LDO rails are off in ship mode. The DRV5032 must be powered from **VSYS** (always connected to battery) so it can monitor the magnet and drive SHPHLD correctly. At 1.8 µA quiescent, a 500 mAh cell lasts ~32 years in ship mode — negligible shelf drain.

**No pull-up resistor needed on SHPHLD:** Push-pull output drives the line firmly HIGH or LOW without an external pull-up.

**Root cause of Q4 reset loop (original schematic):** Q4 was wired as an inverter between the Hall sensor and SHPHLD. This flipped the polarity — SHPHLD was driven LOW when no magnet was present, preventing ship mode from ever latching. With a magnet, Q4 drove SHPHLD HIGH — the opposite of what wakes the PMIC. Completely backwards.

**v2 fix:** Remove Q4. Connect DRV5032 output directly to SHPHLD. Power DRV5032 VDD from VSYS.

### 16.9 — known validation gaps / handoff notes for firmware engineer (2026-05-05)

These items were discovered or confirmed during bring-up but were **not resolved**. They must be addressed by the application firmware engineer before the platform is considered production-ready.

#### 16.9.1 — 9151 test firmware requires L15 to be erased first

During bring-up, running the 9151 test firmware **only works reliably when the nRF54L15 flash is erased** (or the L15 is held in reset). Reason: the two chips share multiple pins (SPI3 MOSI/MISO/SCK, i²c2 SDA/SCL, and several inter-MCU signals). If L15 firmware is present and running, it may actively drive those shared pins, causing bus contention and corrupting 9151-side reads (or vice versa).

**Firmware engineer action required:** Define a bus arbitration protocol between the 9151 and L15 before both sides run simultaneously. At minimum, add a power-on handshake (e.g. via the inter-MCU UART once that path is validated) so the two chips agree on which one owns each bus before any peripheral accesses happen. Until that protocol exists, always erase the L15 before flashing and testing the 9151 application, and vice versa.

#### 16.9.2 — Wi-Fi scan validated via 9151 only; RF switch power path not fully proven end-to-end

The nRF7000 Wi-Fi scan was tested with the 9151 as the host, with the BGS12SN6E6 RF switch VC line **manually wired to VCC** on the bench. During those tests only **5 GHz APs** were visible; 2.4 GHz APs were not seen. This is likely because the RF switch antenna routing was not in the correct state for the 2.4 GHz path without proper firmware-controlled sequencing.

The production-intended RF path is:
1. L15 drives **RFFE_BLE_WIFI_ENABLE (P1.10) HIGH** → RV2C010UNT2L gate FET closes → RF switch VDD powered.
2. The 9151 (or L15) drives **SW_CTRL0** to route the antenna to either the nRF7000 (Wi-Fi) or the L15 (BLE) path.

This end-to-end path (L15 firmware powering the switch, correct SW_CTRL0 state, Wi-Fi scan via 9151, antenna correctly routed) was **never validated as an integrated system** during bring-up. The L15 firmware now drives P1.10 HIGH at boot (committed in 59c496f), but the full RF switch → both-band Wi-Fi scan sequence still needs to be verified.

**Firmware engineer action required:**
- Identify which chip drives SW_CTRL0 and add that GPIO control to its firmware.
- With L15 running (P1.10 HIGH, SW_CTRL0 set to Wi-Fi position) and 9151 running the Wi-Fi scan, re-run the scan and verify both 2.4 GHz and 5 GHz APs are visible.
- Document the correct SW_CTRL0 state for BLE vs Wi-Fi antenna routing.

#### 16.9.3 — inter-MCU UART (LP-UART) not validated

The nrf-sw-lpuart link between 9151 UART0 and L15 uart30 (REQ=P0.31/P1.02, RDY=P0.30/P1.03, TX/RX crosslinked) was **not successfully validated** during bring-up. The L15 side showed `<err> lpuart: Empty receiver state:4` — the REQ/RDY handshake completed but zero UART bytes arrived from the 9151. Root cause: the 9151-side `UART_0_NRF_HW_ASYNC` Kconfig approach used in `prj.conf` is **deprecated in NCS 3.3.0**; the correct approach is a `timer = <&timer2>;` DT phandle property on the uart0 node.

**Firmware engineer action required:**
- In the 9151 board DTS, add `timer = <&timer2>;` to the `&uart0` node (UARTE async requires a hardware timer for RX timeout; the phandle replaces the old `UART_0_NRF_HW_ASYNC_TIMER=2` Kconfig).
- Enable `timer2` with `status = "okay"` in the board DTS (it is disabled by default in `nrf91_peripherals.dtsi`).
- Remove the deprecated `CONFIG_UART_0_NRF_HW_ASYNC`, `CONFIG_UART_0_NRF_HW_ASYNC_TIMER`, and `CONFIG_UART_0_NRF_ASYNC_LOW_POWER` lines from `prj.conf`.
- Re-test the LP-UART link: 9151 sends a test frame, L15 RTT log should show the received bytes without the `Empty receiver state:4` error.

### 16.10 — MPPT validation: wrong passive values root cause found and fixed (2026-05-05)

**Background:** The nPM1300 includes an MPPT (Maximum Power Point Tracking) charger input for solar/PV panels. During earlier bring-up sessions the MPPT was left as unvalidated (step 14 parked list) — it was suspected to be related to the BUCK2 overvolt incident but was not directly tested.

**Root cause:** The MPPT threshold voltage is set by a resistor divider on the nPM1300's VPV sense input. The passive component values chosen in the v1 schematic were incorrect — they set the MPPT trigger voltage **too low**. With the threshold below the actual panel (or test supply) voltage in all realistic conditions, the MPPT algorithm never found a valid operating point and never engaged the charger input. The MPPT appeared broken but the chip itself was fine.

**Fix:** Corrected the resistor values to place the MPPT threshold at the expected operating point for the target PV panel voltage. No firmware or DTS change required — this is purely a passive-component value fix on the board.

**Result:** After updating the passives, MPPT triggers correctly and the charger operates as intended. Validated on bench (2026-05-05).

**v2 schematic action:** Update the MPPT resistor divider to the corrected values. Cross-reference the nPM1300 datasheet MPPT resistor calculation section to confirm values are correct for the chosen panel's Vmpp before layout.

#### 16.10.1 — final validated resistor BOM (all 1 % thin-film E96)

| Designator | Function | Value |
|---|---|---|
| R4 | OV top (to VRDIV) | 4.22 MΩ |
| R5 | OV bottom (to GND) | 3.92 MΩ |
| R6 | UV top | 6.04 MΩ |
| R7 | UV bottom | 3.92 MΩ |
| R14 | OK bottom | 3.57 MΩ |
| R1 | OK middle | 5.76 MΩ |
| R2 | OK top | 0.665 MΩ |
| R8 | MPPT top | 4.02 MΩ |
| R9 (**remove R16**) | MPPT bottom | 15.8 MΩ single resistor |

#### 16.10.2 — resulting thresholds

| Threshold | Value | Notes |
|---|---|---|
| VBAT_OV nominal | 3.89 V | Worst case ≤ 4.03 V — well clear of 4.20 V abs max |
| VBAT_UV | 3.18 V | Hard battery cutoff |
| VBAT_OK falling | 3.27 V | Signal MCU to shed load |
| VBAT_OK rising | 3.50 V | Signal MCU to resume — 230 mV hysteresis prevents chattering |
| MPPT ratio | 80 % of solar Voc | Standard fixed-ratio MPPT; appropriate for most small-panel designs |

#### 16.10.3 — cycle-life projection

At a 3.90 V charge target (set by OV threshold) and a 3.18 V hard cutoff (UV threshold), the usable depth of discharge is conservative. Expected result: **3 000–5 000 charge cycles before cell drops to 80 % capacity**. For a solar-powered wireless sensor with roughly one full cycle per day, that translates to **8–14 years of cell life** — almost certainly exceeding the service life of the surrounding electronics. This is a good design outcome and no further tuning of the thresholds is warranted.

### 16.8 — closing

Bring-up phase is **complete**. Every Nordic chip on the board has been verified alive, on its bus, doing its primary radio function. Every supply rail is verified and stable. Every peripheral that mattered for "is the platform functional" has been characterised. The hardware quirks are all known, the v2 deltas are all specified, the application engineer has a working starting point.

This is the moment to step back from the bench, hand the steps.md + memory base to whoever takes the next phase, and let them start building the actual product on top of the validated radios.
