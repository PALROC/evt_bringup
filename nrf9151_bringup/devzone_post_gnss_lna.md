# DevZone follow-up — GNSS LNA + SPI3 flash on EVT2 (combined post)

> One follow-up post to Ressa covering both open EVT2 issues.
> He already has the board context from earlier threads.
> Attach schematic snippets for the GNSS section AND the SPI3/flash
> section when posting.

---

**Title:** EVT2 follow-up: GNSS LNA never enables (DK fixes with same antenna) + W25Q128JV reads 0x00 on SPI3 with L15 erased and pull-ups added

Hi Ressa — follow-up on the custom nRF9151 (palroc nova EVT2) board.
Two open issues, both with new data since the last thread.

**Setup recap (shared by both issues):**

- mfw `mfw_nrf91x1_2.0.4`, NCS v3.0.2 / Zephyr 4.0.99
- nPM1300 PMIC. Measured rails: BUCK1, BUCK2, LDO1 all solid 3.3 V
- nRF9151 on a custom board alongside an nRF54L15 (separate SiP)
- For all tests below the L15 is **erased** (no firmware, GPIOs
  out-of-reset = high-Z) so it can't contend on the shared SPI3 bus

---

## Issue 1 — GNSS LNA / COEX0 never enables; nRF9151 DK with same antenna fixes outdoors

### LNA topology

- LNA VCC ← nPM1300 LDO1 (3.3 V, always-on, LDO mode, **measured 3.3 V**)
- LNA VEN ← nRF9151 COEX0 via **1 kΩ series + 1 MΩ shunt to GND**
- LNA in  ← antenna connector (external active GNSS antenna; same one
            used on the DK's U.FL for the A/B)
- LNA out → nRF9151 GNSS RF input
- **LNA part: [fill in part number from schematic before posting]**

### Firmware: stock NCS `samples/cellular/gnss` on BOTH boards

To match what the DK does at boot, we added one `Kconfig.defconfig` line
on the custom board so `MODEM_ANTENNA` auto-enables the same way it
does on the official DKs:

```kconfig
config MODEM_ANTENNA
    default y if NRF_MODEM_LIB
```

Boot RTT on EVT2 confirms the on-init hook fires BEFORE any modem
activity, exactly like the DK:

```
*** Booting nRF Connect SDK v3.0.2-89ba1294ac9b ***
<inf> gnss_sample: Starting GNSS sample
<dbg> modem_antenna: set_antenna_configuration:
      Setting configuration: AT%XCOEX0=1,1,1565,1586
```

### A/B result (same sample, same antenna, same balcony spot, minutes apart)

| Board       | Tracking | Using | CN0          | Fix                          |
|-------------|----------|-------|--------------|------------------------------|
| nRF9151 DK  | 4        | 4     | 26–37 dB-Hz  | YES                          |
| EVT2 custom | **0**    | **0** | **0 dB-Hz**  | never                        |

EVT2 `Tracking: 0` is steady — not noisy zero, just no SVs at all.

### Earlier datapoint that's relevant

In our own bring-up app we previously had `AT%XCOEX0` fired LATE (after
modem activity); measured **COEX0 = 0 V** at the LNA VEN. Moving the
command to `NRF_MODEM_LIB_ON_INIT` (via `modem_antenna`) was the
obvious fix — but with that fix in place EVT2 still tracks nothing.

### VNA measurements (LiteVNA, 1.5-1.65 GHz, SOL-calibrated)

Attached in `vna_screenshots/`:

- `vna_evt2_antenna_connector_no_antenna.png` — S11 at the EVT2
  antenna connector, board ON (LNA powered + COEX0 asserted),
  GNSS sample running, no antenna plugged in. Smith chart shows a
  large loop far from center at 1.575 GHz — but the receiver still
  tracks up to 12 SVs through the connector pin acting as a stub
  antenna, with one peak at **41 dB-Hz**. So the LNA chain is alive.
- `vna_evt2_antenna_connector_with_antenna.png` — same probe point
  with the real GNSS antenna plugged in.
- `vna_dk_antenna_connector_for_comparison.png` — same measurement
  on the nRF9151 DK as a reference, same VNA setup.

The interesting delta: with NO antenna and just a VNA cable as a
random wire, EVT2 indoors tracks 12 SVs and hits 41 dB-Hz briefly.
With the actual GNSS antenna plugged in, EVT2 tracks 0 SVs (the
A/B above). The DK with the same antenna tracks 4 and fixes.

### Questions

1. With the on-init log above confirming `AT%XCOEX0=1,1,1565,1586`
   was sent before modem activity on mfw 2.0.4, is there any other
   reason COEX0 wouldn't go high while GNSS is running? CFUN ordering,
   GNSS sub-mode quirks, "RF-off inversion" semantics — anything that
   could leave COEX0 at 0 V even after a successful config?
2. Any red flags in the LNA topology above vs Nordic's reference GNSS
   front-end? COEX0 swings to VDD_GPIO — can you confirm what supply
   VDD_GPIO is referenced to on the 9151 SiP so we can verify our
   COEX0 high-level matches the LNA's VEN min threshold
   (VCC − 0.3 V = 3.0 V with VCC = LDO1 = 3.3 V)?
3. Given the VNA shows a wildly mismatched input at the antenna
   connector AND the real antenna gives 0 SVs while a random cable
   gives 12 SVs and 41 dB-Hz peaks — does the Smith trace look like
   a known antenna-matching network issue between the U.FL/SMA and
   the LNA input on the path between connector and LNA input?

---

## Issue 2 — W25Q128JV SPI3 reads 0x00 with L15 erased and /CS /WP /HOLD pulled high

### Setup

- W25Q128JV (SOIC-8) on the nRF9151 SPI3:
  - SCK = P0.13, MOSI = P0.15, MISO = P0.14, CS = P0.12
  - Pinctrl matches the original palroc board that read `EF 40 18`
    cleanly in earlier bring-up.
- SPI bus is **shared with the nRF54L15** (its spi00) — the L15 is
  the secondary master, but for this test the L15 is **erased entirely**
  (no firmware, all GPIOs out-of-reset = high-Z).
- BUCK2 = 3.3 V at the flash VCC (pin 8), measured.

### What we did

1. Soldered 10 kΩ pull-ups on **/CS (pin 1)**, **/WP (pin 3)**,
   **/HOLD (pin 7)** to 3.3 V — these were not on the board originally
   and /WP / /HOLD were floating.
2. Erased the nRF54L15 to fully eliminate shared-bus contention.
3. Ran our `0x9F` JEDEC ID read: 1 MHz, mode 0, a single
   `spi_transceive()` for command + 3 dummy bytes, CS asserted for the
   full 4-byte exchange — verified code is textbook-correct (the same
   code read `EF 40 18` originally before the contention era).

### Result (3 attempts)

```
[00:00:17.623,809] flash JEDEC attempt 1/3: 00 00 00 (unexpected; will retry)
[00:00:17.634,033] flash JEDEC attempt 2/3: 00 00 00 (unexpected; will retry)
[00:00:17.644,256] flash JEDEC attempt 3/3: 00 00 00 (unexpected; will retry)
```

We're past the "obvious" causes — voltage, mode, pinctrl, firmware,
L15 contention, control-pin pull-ups. With the L15 erased and the
W25Q's control inputs all confirmed high, the chip should respond.
It doesn't.

### Working hypothesis

During the long debugging where the L15 was booting freely and the
two MCUs were contending on the shared MISO, the flash's DO driver
was electrically damaged — `0x00 0x00 0x00` is the "chip alive, output
stage dead" fingerprint. Matches the earlier boards' history
(different garbage patterns per board, all on the same SPI3 bus
topology).

### Questions

1. Anything else worth checking from the firmware/SDK side before we
   conclude this is hardware damage and replace the chip? Any
   W25Q-specific gotcha (deep power-down state, lock bits, anything
   we haven't considered)?
2. For the next board spin: our plan is to hardware-arbitrate the
   shared SPI3 bus so only one master drives it at a time (or split
   it entirely). Does Nordic have a recommended pattern for two-master
   shared SPI on the 91 + 54L pair, or is "don't share — use separate
   buses + bridge through the inter-MCU UART link" the only sane
   answer?

Thanks!
