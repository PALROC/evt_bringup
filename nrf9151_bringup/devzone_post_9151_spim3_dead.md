# DevZone post — nRF9151 SPIM3 (and SPIM1) silently fail on a custom board while bit-bang on the exact same pins reads the flash perfectly

> Final draft for Ressa. The story is genuinely weird and not yet
> resolved. Posting to ask for diagnostic help — not to claim a
> root cause. Attach: the three test apps, the custom board files,
> RTT captures, schematic snippet of the shared SPI3 net.

---

**Title:** Reproduced on stock DKs: when an nRF54L15 actively polls SPI on a shared bus, an nRF9151's SPIM3 silently breaks and never recovers (transfers keep returning `ret=0` with `rx=00`) — same failure mode as our custom board

## UPDATE — reproducer on stock DK hardware (added at the end)

We narrowed the EVT2 failure to a clean DK-pair reproducer. Two stock
Nordic DKs (nRF9151 DK + nRF54L15 DK) plus a 5-wire jumper bridge,
plus one wire from L15 P1.10 to 9151 P0.20 (the 9151 DK's onboard
GD25 /CS line, so both MCUs can assert /CS on the same chip — the
EVT2 topology).

Behavior on the 9151 DK's RTT:
```
iter 1: JEDEC c8 65 19  PASS         <- L15 still booting
iter 2: c8 65 19  PASS
iter 3: c8 65 19  PASS
iter 4: c8 65 19  PASS
iter 5: 00 00 00  FAIL  (no clocks)  <- L15 finished boot, started polling
iter 6: 00 00 00  FAIL
iter 7..n: 00 00 00  FAIL            <- never recovers
```

The L15 just polls SPIM00 at 10 Hz (JEDEC reads, ~32 µs per transfer).
The transition from "9151 reads cleanly" to "9151 returns all-zero
forever" happens **precisely when the L15 starts transferring**, and
the 9151's transfers don't recover even between L15 transactions —
SPIM3 ends up wedged in a state it doesn't return from.

This is the same failure mode we saw on EVT2: `spi_transceive` keeps
returning `ret = 0`, no driver errors, just no clocks on the bus.
Bit-bang on the same pins reads the same chip correctly on EVT2, and
on the DKs in this reproducer we can flash a different firmware that
reads the GD25 from the 9151 cleanly as long as the L15 is erased.

The full reproducer (board files, both apps, READMEs) is in the
attached zip under `test_apps/` and the DK-pair-specific source is
also in this thread's previous attachments.

---

**Title (original, for context):** Custom nRF9151 board: every Zephyr SPI transfer returns `ret=0` with `rx=00 00 00`, on SPIM3 AND on SPIM1, while bit-bang on the same physical pins reads the W25Q128JV flash perfectly — looking for what we're missing

Hi Ressa — follow-up on the EVT2 SPI3 / flash thread. The earlier
chip-damage and bus-contention theories are dead (the chip is fully
alive, proven below). What remains is a really strange failure pattern
on the 9151 side that we haven't been able to root-cause. Looking for
help understanding what's going on.

---

## TL;DR

1. The W25Q128JV flash on EVT2 SPI3 is **fully alive**. Confirmed in
   two completely independent ways:
   - From the nRF54L15 via its `spi00`: full erase / page-program /
     persistence test working across resets.
   - From the nRF9151 via **GPIO bit-bang on the same 4 pins**:
     reads JEDEC `EF 40 18` AND reads back 8 bytes the L15 had
     written to address 0x000000 a few minutes earlier. End-to-end
     cross-MCU shared-flash storage works.
2. The Zephyr SPI driver, asked to talk to the same chip on the same
   pins from the same 9151, always returns `ret = 0` with `rx = 00 00
   00`. No errors, no warnings, just no clocks.
3. We tried moving the flash to **SPIM1** instead of SPIM3 via overlay
   (with the nRF7000 disabled). Verified the overlay applied
   (`>>> SPI device name from DT: spi@9000`). **Same `00 00 00` result.**
   But — Wi-Fi works on SPIM1 in our full bring-up firmware, so we
   know SPIM1 silicon is functional. So the "SPIM3 silicon is dead"
   theory doesn't hold either: somehow SPIM1 also fails in this
   minimal test app.
4. We tried building in **secure mode** (no `/ns`) so TF-M can't
   block the peripheral. Same `00 00 00`.
5. **Working hypothesis we haven't yet verified cleanly** (RTT
   keeps eating our diagnostic output): our own test app
   reconfigures pins as GPIO for the bit-bang section, and the SPIM
   peripheral doesn't reclaim them — so transfers happen "internally"
   but never reach the pins. This is testable but we haven't pinned
   it down yet. See "Open hypothesis" below.

So the picture is: chip is fine, board is fine, peripheral is
**probably** fine but we can't currently get any Zephyr SPI transfer
to actually clock anything on this board. Want help understanding
what could put both SPIM3 and SPIM1 into a "drives are silently dropped"
state, and what the canonical way to release pins back to a SPI
peripheral after using them as GPIOs is in NCS.

---

## Setup recap

- Custom board (palroc nova EVT2): nRF9151 + nRF54L15 + W25Q128JV on
  a **shared SPI3 bus** (same physical SCK/MOSI/MISO).
- mfw `mfw_nrf91x1_2.0.4`, NCS v3.0.2 / Zephyr 4.0.99.
- Default target: `nrf9151_nova_evt2/nrf9151/ns`.
- 9151 SPI3 pinctrl: `SCK = P0.13`, `MOSI = P0.15`, `MISO = P0.14`,
  `CS = P0.12` (CS via cs-gpios).
- 9151 SPI1 pinctrl (used by nRF7000 Wi-Fi in the bring-up): different
  pins. For the SPI1 test, we routed it to the flash pins via app overlay.
- L15 was erased during 9151-side tests so it cannot contend on the bus.

---

## Evidence

The test app is `flash_test_evt2/` — minimal Zephyr (only `CONFIG_SPI=y`
and `CONFIG_GPIO=y`, no modem, no networking, no other peripherals).

### Test 1 — GPIO pin health probe (raw GPIO reads before any SPI work)

```
>>>   P0.14 PULL-UP   reads 1  (pin alive)
>>>   P0.14 PULL-DOWN reads 0  (pull-down wins)
>>>   P0.14 NO PULL   reads 0  (floating)
>>>   P0.12 (CS) PULL-UP reads 1  (idles HIGH)
```
All four pins respond to internal pull resistors as expected. No
stuck-low, no shorts, no electrical damage.

### Test 2 — Software bit-bang on the same 4 pins (no SPIM peripheral)

```
>>> BITBANG JEDEC: ef 40 18   *** PASS — chip alive via bit-bang ***
>>> BITBANG all 4 bytes raw: 00 ef 40 18
>>> marker bytes: 50 41 4c 52  07 00 00 00
>>> *** PALR FOUND *** counter=7 (written by the L15 earlier)
```
We send `0x9F` + 3 dummies on the same `P0.13/14/15/12` driven as plain
GPIOs, mode 0, ~125 kHz, in a tight `gpio_pin_set` / `gpio_pin_get` loop.
The chip returns its JEDEC ID perfectly. And then a bit-bang `READ DATA`
(0x03) from address 0x000000 returns 8 bytes that the **nRF54L15 had
written earlier** in a separate test (`flash_test_l15_evt2`). The chip,
the board, and the cross-MCU storage path are all confirmed working at
the GPIO level.

### Test 3 — SPIM3 via Zephyr's SPI driver, same pins, same chip

```c
static struct spi_config cfg_flash = {
    .frequency = 1000000,
    .operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
    .cs = { .gpio = { .port = gpio0, .pin = 12, .dt_flags = GPIO_ACTIVE_LOW }, .delay = 2 },
};
uint8_t tx[4] = { 0x9F, 0, 0, 0 }, rx[4] = { 0 };
spi_transceive(spi3_dev, &cfg_flash, &txs, &rxs);   // returns 0
```
Result every iteration:
```
iter N: JEDEC 00 00 00   ----   (no chip responding)
```
`spi_transceive` returns `0`, no driver errors, but `rx` is always
all zeros. No oscilloscope yet — but the bit-bang reading the chip
perfectly on these exact pins tells us either (a) SPIM3 isn't actually
clocking, or (b) it's clocking but its PSEL has been disconnected.

### Test 4 — Same code on SPIM1 instead of SPIM3, via app overlay

Overlay: disable `&spi3`, disable the nRF7000 child of `&spi1`,
override `&spi1`'s pinctrl to the flash's pins, set `&spi1`'s
`cs-gpios = <&gpio0 12 ...>`. Verified the overlay applied:
```
>>> SPI device name from DT: spi@9000   (= SPIM1)
```
**Same `00 00 00` result.** But SPIM1 silicon is known good — Wi-Fi
works on it in our full bring-up firmware. So both SPIM3 AND SPIM1
fail in this minimal test, even though SPIM1 is provably alive in
another context.

### Test 5 — Secure build (no `/ns`) to rule out TF-M

Same code, built for `nrf9151_nova_evt2/nrf9151` (secure variant).
Same `00 00 00`. TF-M / SPU attribution can't be the cause if even the
secure-domain build fails.

### Test 6 — Memory-mapped register probe

Tried reading SPIM3's `ENABLE` register directly:
```c
volatile uint32_t *spim3 = (volatile uint32_t *)0x4000B000;  /* NS alias */
uint32_t val = spim3[0x500 / 4];
printk(">>> probe RETURNED 0x%08x\n", val);
```
The printk **never reaches its value**. The system hangs for ~5 s,
recovers, and resumes the SPI poll loop. Same in secure mode with
`0x5000B000`. So either:
- Both the NS and S aliases of SPIM3 are blocked from us (which
  would be surprising), or
- The peripheral is in a powered-down / clock-gated state where its
  registers don't respond
- (And we'd expect TF-M to give us a clean fault, not a hang.)

---

## Open hypothesis (untested due to RTT output corruption issues)

Looking at the sequence in our test app:

1. Boot. Zephyr SPI driver runs init, calls `pinctrl_apply_state()` on
   `&spi3` (or `&spi1`). The peripheral's PSEL.* registers point at
   P0.13/14/15.
2. Our `main()` runs the GPIO probe block, calling
   `gpio_pin_configure(gpio0, 14, GPIO_INPUT | GPIO_PULL_UP)`. This
   writes `PIN_CNF[14]`.
3. Our bit-bang block calls `gpio_pin_configure(gpio0, 13, GPIO_OUTPUT_LOW)`
   and similar on the other pins.
4. We call `spi_transceive()`. SPIM's PSEL still points at the pins,
   but `PIN_CNF[*]` has been overwritten by step 3.

**On nRF91, what happens when `PIN_CNF[n]` is written to a pin that's
also claimed by a peripheral's PSEL?** Our suspicion: GPIO's PIN_CNF
dominates, the peripheral signal is "internally generated but never
escapes to the pin," and `spi_transceive` happily completes thinking
nothing went wrong.

We tried to release the pins back to the peripheral by calling
`gpio_pin_configure(gpio0, X, GPIO_DISCONNECTED)` after the bit-bang,
but RTT keeps eating that section of our output (deferred log queue
overflow). We haven't yet been able to verify whether the fix takes.
This is on the to-do for the morning: write a stripped-down app that
does ONLY `spi_transceive`, no GPIO probe, no bit-bang.

---

## What we've ruled out

| Hypothesis | Evidence against |
|---|---|
| Flash chip damaged | L15 reads + writes it cleanly; 9151 bit-bang reads it cleanly |
| L15 contention on shared bus | L15 erased during the 9151 test |
| MISO pad / trace damage | GPIO PULL-UP reads 1, PULL-DOWN reads 0; bit-bang receives chip data |
| SCK / MOSI / CS trace damage | Bit-bang on those pins reaches the chip both ways |
| Wrong pinctrl | Bit-bang uses the SAME pin numbers and works |
| Stale build / old image | `BUILD-TAG` string is in every boot's RTT output |
| TF-M / SPU blocking SPIM3 NS | Secure-mode build also fails |
| "SPIM3 silicon is dead" | SPIM1 fails the same way in our test app, but Wi-Fi works on SPIM1 in bring-up firmware → SPIM1 silicon is fine → the dead-silicon story doesn't hold |
| Shared-instance Kconfig conflict | No other role on instance 3 enabled; SPIM1 test had nRF7000 disabled |
| Voltage / supply | BUCK2 = 3.3 V solid at the flash VCC pin; chip responds via bit-bang at the same rail |

---

## Our questions

1. **What is the canonical pattern in NCS for using pins as both GPIO
   (e.g. for a probe or workaround) AND as a SPIM peripheral signal
   within the same app?** Does Zephyr / nrfx automatically reclaim pins
   from GPIO when a SPIM transfer is initiated, or do we need to
   explicitly re-apply pinctrl / disconnect GPIO before each transfer?

2. **What can put SPIM3 (and SPIM1, in the same app) into a state
   where `spi_transceive()` returns `ret = 0` but no SCK / MOSI ever
   appears on the pins, with no driver error?** This is the deepest
   puzzle: even after the secure-mode test (ruling out TF-M) and the
   SPIM1 test (ruling out single-instance damage), both peripherals
   silently produce nothing. Is there a Kconfig, a system-wide power
   state, or a clock source that needs to be set up for SPIM beyond
   `CONFIG_SPI=y` and `CONFIG_NRFX_SPIM3=y` on the nRF9151 /ns?

3. **Memory-mapped register reads of SPIM3 (both 0x4000B000 NS and
   0x5000B000 S aliases) hang the CPU.** Is that consistent with the
   peripheral being clock-gated until its driver enables it, or does
   that suggest something more concerning? On the L15 we'd get a
   clean SECURE FAULT for blocked accesses; we never see anything
   here, just a ~5 s hang and recovery.

4. **The bit-bang workaround is solid.** We've demonstrated reads,
   writes, erase, and persistence across power cycles via GPIO
   bit-bang on the SPI3 pins, with the L15 doing the writes and the
   9151 doing the reads. Is there a Nordic-blessed pattern for
   reliable GPIO bit-bang SPI on the nRF9151 (drive strength
   configuration, max rate, etc.) we can fall back to for production
   if the underlying SPIM issue isn't quick to resolve?

---

**Materials I'll upload to this thread:**

- `evt_bringup/flash_test_evt2/` — the 9151-side test app (full source,
  including bit-bang + the failing SPI driver path)
- `evt_bringup/flash_test_l15_evt2/` — the L15-side write/persistence test
- `palroc_nova/boards/palroc_nova/nrf9151_nova_evt2/` — custom board files
- `palroc_nova/boards/palroc_nova/nrf54l15_nova_evt2/` — L15 side board files
- Full RTT capture
- Schematic snippet of the shared SPI3 net + flash

Happy to jump on a call if it's faster to walk through together.

Thanks!
