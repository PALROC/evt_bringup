# Wi-Fi-on-L15 handover — debug state (paused 2026-05-22)

Paused mid-bring-up to test EVT2 with the known-working "Wi-Fi on 9151"
baseline (the `main` branch). Resume from this `dev` branch by checking
out `dev` in both `evt_bringup` and `palroc_nova`.

## What works on `dev` today

- **PMIC handover succeeded.** The L15 is now the i²c master for the
  npm1300, and the regulator driver applies `BUCK1 = BUCK2 = 3.3 V` at
  boot. Verified on hardware: multimeter reads 3.3 V on **both** rails.
  This is the fix for the original `VBAT < VDDIO` (1.8 V vs 3.3 V) that
  was breaking the nRF7000.
- **Power path L15 → nRF7000 confirmed.** Driving P1.00 (BUCK_EN) +
  P1.01 (IOVDD_CTRL) HIGH directly from the app pulled ~120 mA on the
  PPK2, proving the L15 pins reach the nRF7000 load switches on EVT1.
- **9151 stops contending.** Its `&i2c2` and `&spi1` are disabled at the
  board level; `npm1300_probe()` and `i2c_probe.c` are out of the
  9151's build.
- **i²c20 SCL/SDA swap fixed** in the nova L15 pinctrl (was SCL=P1.4 /
  SDA=P1.5, corrected to SDA=P1.4 / SCL=P1.5 to match the 9151's
  P0.09=SDA / P0.08=SCL).
- **L15 fits BLE + Wi-Fi in RAM**: `CONFIG_TFM_PROFILE_TYPE_MINIMAL=y`
  + `CONFIG_HEAP_MEM_POOL_SIZE=32768`.

## What still doesn't work

nRF7000 RPU comms still fails at boot:
```
[00:00:00.015,425] <err> wifi_nrf_bus: Error: RPU comms test: sig failed:
                                       expected 0x42000020, got 0x80aaaa
```

Read carefully: the failure happens at **t = 15 ms**. The npm1300
driver runs at SYS_INIT and its i²c writes to BUCK1 don't necessarily
finish before the nrf70 driver (also SYS_INIT) tries SPI. Likely
sequence:

1. SYS_INIT → npm1300 starts pushing BUCK1 = 3.3 V (async i²c).
2. SYS_INIT → nrf70 powers BUCK_EN/IOVDD HIGH and immediately tries SPI.
3. nRF7000 still sees VBAT = 1.8 V (datasheet violation) → RPU comms
   garbage `0x80aaaa`.
4. By the time `main()` runs and we measure, the i²c write *has*
   completed → multimeter shows 3.3 V. So the static state is fine, the
   race is during driver init.

The driver doesn't retry RPU comms — it just gives up, which is why the
chip stays unreachable for the rest of the run even though BUCK1 is now
correct.

## Most likely next step (try first tomorrow)

Force `nrf70` driver init to happen **after** the regulator driver. Two
ways:

1. **Bump nrf70 init priority** via Kconfig so it inits after
   `REGULATOR_NPM1300`. The npm1300 regulator driver runs at
   `POST_KERNEL` priority ~75 by default; force nrf70 to run later, e.g.
   `CONFIG_WIFI_NRF70_INIT_PRIORITY=90`.

2. **Disable `&nrf70` at the DT level** and init Wi-Fi explicitly from
   `main()` after the PMIC is confirmed up + a short settle delay. The
   driver exposes manual init APIs, or we can leverage the
   `bucken-gpios` to gate the chip until ready.

Option 1 is the smaller change. Option 2 is the more robust pattern
(the same `RFFE_BLE_WIFI_ENABLE` lives in app code already).

## Other things worth verifying

- The `pmic_master` test result didn't appear in the RTT log — the
  paste was truncated by an RTT buffer overflow (`***` marker). Reflash
  with RTT attached from `--reset` to confirm `test_pmic_master`
  actually passes. Should print `PASS  i2c20 + npm1300 up; expect
  BUCK1=BUCK2=3.3 V on multimeter`.
- MISO byte changed from `0xa8aaaa` (dead) to `0x80aaaa` (slightly
  responsive) between the two PMIC states — corroborates "VBAT was low
  during SPI test."
- The L15's i²c bus needs external pull-ups (~4.7 kΩ). Schematic should
  confirm they're populated; if not, the npm1300 might ACK marginally
  and we'd see odd PMIC behavior over time.

## Files changed on `dev` vs `main`

Commits on `palroc_nova` dev (relative to `main`):
- `830969a` Move nrf70 SPI from 9151 to L15
- `38e5b63` Disable 9151 spi1 (stop contending on shared SPI)
- `a54f417` PMIC ownership: hand npm1300 from 9151 to L15
- `0c9b21c` Remove orphaned npm1300 DT child from 9151

Commits on `evt_bringup` dev (relative to `main`):
- `17336c6` wifi_probe: raw-pin GPIO (not DT spec) for power pins
- `d73609c` Fit BLE + Wi-Fi in RAM (TFM minimal + smaller heap)
- `e6b780f` Migrate Wi-Fi firmware from 9151 to L15
- `3535ccd` PMIC ownership firmware halves
- `d56699e` Drop i2c_probe from 9151
- `b1632c1` Replace gpio_park test with pmic_master test

## Rolling back to known-good

```bash
# Both repos, checkout main:
cd /path/to/evt_bringup && git checkout main
cd /path/to/palroc_nova && git checkout main
```

The `main` branch in both has the original Wi-Fi-on-9151 setup that's
validated end-to-end.
