# DK-pair SPI reproducer — does the EVT2 SPIM3 failure happen on stock DKs too?

This folder pairs two minimal apps that recreate the EVT2 shared-SPI
topology on stock Nordic DK hardware:

- `9151dk_master/` — runs on the **nRF9151 DK**. Reads the onboard
  GD25WB256 flash via SPI3 every 5 s. Prints JEDEC ID.
- `l15dk_observer/` — runs on the **nRF54L15 DK**. By default does
  nothing but boot (passive observer on the bus). A `#define` flips it
  to active contention mode for the worst-case test.

The goal is to find out whether the EVT2 failure (Zephyr SPI driver
returns `00 00 00` when a second MCU is sitting on the same SPI bus)
is reproducible on reference hardware. If yes, we have a clean
reproducer for Ressa. If no, the EVT2 PCB itself is the unique factor.

## Test matrix

| Test | 9151 DK | L15 DK | Expected on 9151 RTT | What it proves if it fails |
|------|---------|--------|---------------------|-----------------------------|
| **A** | flashed with `9151dk_master` | not wired at all | `ONBOARD JEDEC c8 65 19 PASS` | (baseline — already passed) |
| **B** | flashed with `9151dk_master` | wired but **erased** | should also PASS | Mere physical presence of a second nRF MCU on the bus breaks it |
| **C** | flashed with `9151dk_master` | flashed with `l15dk_observer` (IDLE mode) | should still PASS | Full Zephyr boot on the L15 (with TF-M init etc.) — but no SPI activity — breaks the 9151's transfers |
| **D** | flashed with `9151dk_master` | flashed with `l15dk_observer` (POLL mode) | likely FAILS due to bus contention | Recreates the EVT2 production scenario; if it matches EVT2's behavior, we've reproduced the bug on DKs |

## Wiring (5 jumper wires between the DK Arduino headers)

```
nRF9151 DK              nRF54L15 DK
  D13 (P0.13 SCK)  <--->  P2.1
  D11 (P0.11 MOSI) <--->  P2.2
  D12 (P0.12 MISO) <--->  P2.4
  GND              <--->  GND   <-- common ground is CRITICAL
```

**No CS wire to the L15 DK.** The onboard GD25 flash CS lives on the
9151 DK's internal P0.20 (not on the Arduino header). The 9151 owns
chip-select; the L15 sits on the bus as a peripheral observer or
co-driver but cannot select the flash itself.

## Procedure

### Test A (baseline, no L15 wired)

```bash
# 1. Find your J-Link serials
nrfjprog --ids

# 2. Flash the 9151 DK
cd 9151dk_master
west build -p -b nrf9151dk/nrf9151/ns -d build
west flash -d build --snr <9151_DK_serial>

# 3. Watch RTT — expect:
#    ONBOARD JEDEC c8 65 19  PASS  GigaDevice GD25WB256
```

### Test B (L15 erased and wired)

1. Wire the 5 jumpers per the table above.
2. Erase the L15 DK:
   ```bash
   nrfjprog --eraseall -f NRF54L --snr <L15_DK_serial>
   # Verify:
   nrfjprog --memrd 0x00000000 --n 16 -f NRF54L --snr <L15_DK_serial>
   # Should show ff ff ff ff ...
   ```
3. (9151 DK already has `9151dk_master` from test A.) Power cycle both
   DKs and watch RTT on the 9151.

### Test C (L15 booting but idle)

1. Wiring stays the same as test B.
2. Build `l15dk_observer` in default IDLE mode and flash it on the L15
   DK:
   ```bash
   cd l15dk_observer
   west build -p -b nrf54l15dk/nrf54l15/cpuapp/ns -d build
   west flash -d build --snr <L15_DK_serial>
   ```
3. Re-run the 9151 DK test, watch RTT.

### Test D (L15 actively driving SCK/MOSI — worst case)

1. Edit `l15dk_observer/src/main.c`, change `#define MODE_IDLE 1` to
   `#define MODE_IDLE 0` (selects POLL mode).
2. Rebuild and reflash `l15dk_observer` on the L15 DK.
3. Re-run the 9151 DK test.

## What each outcome means

| Result | Conclusion |
|--------|------------|
| Tests A-C all PASS, D fails | EVT2's failure is about active contention, not the shared topology itself. Bit-bang or arbitration is the right fix. |
| Test C fails (L15 booted but no SPI activity) | The L15 just being alive on the bus is enough — clean reproducer for Ressa. Strong evidence the failure is in NCS / SDK behavior, not EVT2 hardware. |
| Test B fails (L15 erased on the bus) | The wire-stub alone breaks signal integrity. Less likely but possible — would suggest EVT2's PCB layout is the issue. |
| All four PASS | EVT2's failure is unique to that PCB. We pivot to scoping EVT2 directly. |

## Notes

- L15 DK `cpuapp/ns` variant runs TF-M. The L15-side test in EVT2
  (`flash_test_l15_evt2`) needed `CONFIG_NRF_SPIM00_SECURE=n` and
  `CONFIG_NRF_GPIO2_SECURE=n` to release peripherals to NS — the same
  release configs are included in `l15dk_observer` for the POLL mode.
- All output is `printk` (not LOG_INF) because the Zephyr log subsystem
  has been eating output on this debug — same approach we settled on
  for `flash_test_evt2`.
