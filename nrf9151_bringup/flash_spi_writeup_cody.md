# EVT2 flash: 9151 couldn't read it via Zephyr SPI driver, but raw HAL works

Following up on what I sent over chat — here's the technical version.

## Symptom

W25Q128JV on the shared SPI3 bus (9151 + L15 + flash). NCS v3.0.2 /
Zephyr 4.0.99 on the 9151 side.

- **L15 → flash**: works perfectly. Read, write, erase, persistence
  across power cycles, all good.
- **9151 → flash via Zephyr `spi_transceive()`**: returns `ret=0`
  instantly with `rx = 00 00 00`. No driver error, no log warning, no
  clocks on the wire. The kicker: the transfer reports completing in
  `0 ms` — so it's not a busy timeout, the driver is silently
  early-returning without actually doing anything.

Same chip, same physical bus, same SPI3 silicon. Different result
depending on which MCU was driving and which API.

## What it isn't

Ruled out one by one:

- **Chip damage** — L15 reads / writes / persistence all clean.
- **Trace damage** — bit-bang on the same 4 pins from the 9151 (just
  toggle GPIOs, no SPIM peripheral involved) reads `EF 40 18` cleanly
  at every speed up to ~1 MHz.
- **Bus contention / shared-topology design issue** — reproduced the
  same failure on a stock nRF9151 DK + nRF54L15 DK pair with 6 jumper
  wires bridging SCK/MOSI/MISO/GND + shared /CS. Not unique to our PCB.
- **SPIM3 silicon** — see below, raw HAL gets the chip responding
  perfectly.

## What it is

The 9151's SPIM3 peripheral itself is fine. The Zephyr/nrfx SPI driver
layer (`spi_nrfx_spim.c` → `nrfx_spim.c`) is the broken piece.

Wrote a test app that programs SPIM3 directly through Nordic's
`nrf_spim_*` HAL — no Zephyr SPI subsystem, no nrfx_spim driver:

```c
#include <hal/nrf_spim.h>

static uint8_t tx_buf[4] __aligned(4) = { 0x9F, 0, 0, 0 };
static uint8_t rx_buf[4] __aligned(4);

NRF_SPIM_Type *spim = NRF_SPIM3;

/* one-time setup */
spim->ENABLE = 0;
spim->PSEL.SCK  = 13;            /* P0.13 */
spim->PSEL.MOSI = 15;            /* P0.15 */
spim->PSEL.MISO = 14;            /* P0.14 */
/* nRF91 SPIM has no PSEL.CSN — CS is GPIO-driven manually */
spim->FREQUENCY = NRF_SPIM_FREQ_1M;
spim->CONFIG    = 0;             /* mode 0, MSB-first */
spim->ENABLE    = 7;

/* per transfer */
spim->TXD.PTR    = (uint32_t)tx_buf;
spim->TXD.MAXCNT = sizeof(tx_buf);
spim->RXD.PTR    = (uint32_t)rx_buf;
spim->RXD.MAXCNT = sizeof(rx_buf);
spim->EVENTS_END = 0;

gpio_pin_set(gpio0, CS_PIN, 0);       /* CS low */
spim->TASKS_START = 1;
while (spim->EVENTS_END == 0) { ; }   /* fires in ~15 us */
gpio_pin_set(gpio0, CS_PIN, 1);       /* CS high */

/* rx_buf now has 00 EF 40 18 = W25Q128JV JEDEC ID */
```

Full register dump after the transfer:

```
ENABLE         = 0x00000007    (enabled)
PSEL.SCK       = 0x0000000d    (P0.13, connected)
PSEL.MOSI      = 0x0000000f    (P0.15)
PSEL.MISO      = 0x0000000e    (P0.14)
FREQUENCY      = 0x10000000    (1 MHz)
CONFIG         = 0x00000000    (mode 0, MSB-first)
TXD.PTR=0x2000ca20 MAXCNT=4 AMOUNT=4
RXD.PTR=0x2000eee0 MAXCNT=4 AMOUNT=4
EVENTS_END=1   EVENTS_STARTED=1
EVENTS_ENDTX=1 EVENTS_ENDRX=1
rx_buf raw: 00 ef 40 18
```

Same SPIM3, same pins, same 1 MHz clock that fails with
`spi_transceive()`. Also verified with arbitrary-address `READ DATA`
(0x03) from 0x0 — got back the exact 8 bytes the L15 had written
earlier (`"PALR" + uint32_le counter`), proving the path works for
real flash operations, not just JEDEC.

So the bug is somewhere in:

```
spi_transceive() → spi_nrfx_spim.c → nrfx_spim.c → HAL → silicon
                          ^                ^         ^        ✅ works
                          ^                ^         ✅ works (raw test)
                          ⮕  bug is here somewhere
```

The `xfer=0 ms` + `ret=0` fingerprint suggests something early-bails
without programming the peripheral, or programs `MAXCNT=0` somehow.
Haven't traced through the source to find the exact spot — the
workaround was the priority.

## Production-ready wrapper

The fix is to bypass `spi_transceive` for the flash. Small wrapper
around the raw SPIM3 HAL — drop-in replacement for whatever you'd
have called `spi_transceive` on:

```c
static uint8_t flash_tx_buf[N] __aligned(4);
static uint8_t flash_rx_buf[N] __aligned(4);

static void flash_spim_setup(void)
{
    NRF_SPIM_Type *spim = NRF_SPIM3;
    gpio_pin_configure(gpio0, FLASH_CS_PIN, GPIO_OUTPUT_HIGH);
    spim->ENABLE = 0;
    spim->PSEL.SCK  = 13;
    spim->PSEL.MOSI = 15;
    spim->PSEL.MISO = 14;
    spim->FREQUENCY = NRF_SPIM_FREQ_1M;
    spim->CONFIG    = 0;
    spim->ENABLE    = 7;
}

static int flash_spim_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    NRF_SPIM_Type *spim = NRF_SPIM3;
    if (tx) memcpy(flash_tx_buf, tx, len); else memset(flash_tx_buf, 0, len);
    memset(flash_rx_buf, 0, len);

    spim->TXD.PTR = (uint32_t)flash_tx_buf;  spim->TXD.MAXCNT = len;
    spim->RXD.PTR = (uint32_t)flash_rx_buf;  spim->RXD.MAXCNT = len;
    spim->EVENTS_END = 0;

    gpio_pin_set(gpio0, FLASH_CS_PIN, 0);
    spim->TASKS_START = 1;

    int64_t t0 = k_uptime_get();
    while (spim->EVENTS_END == 0) {
        if (k_uptime_get() - t0 > 100) {
            gpio_pin_set(gpio0, FLASH_CS_PIN, 1);
            return -ETIMEDOUT;
        }
    }
    gpio_pin_set(gpio0, FLASH_CS_PIN, 1);

    if (rx) memcpy(rx, flash_rx_buf, len);
    return 0;
}

static void flash_spim_teardown(void)
{
    NRF_SPIM_Type *spim = NRF_SPIM3;
    spim->ENABLE = 0;
}
```

Call `flash_spim_setup()` once, then any number of `flash_spim_xfer()`
calls (JEDEC, READ DATA, page program, sector erase — all use the
same xfer with different command bytes), then `flash_spim_teardown()`.

`nrf_spim_*` is Nordic's official HAL layer, so this isn't a hack —
it's just skipping the broken glue above it. Same DMA, same speed,
same hardware acceleration as `spi_transceive` would have given us if
it worked.

## Still open

- Which exact function in `spi_nrfx_spim.c` / `nrfx_spim.c` is broken.
  Cheapest way to find it: instrument the Zephyr SPI driver path with
  log lines at each register write, run the failing test, see which
  step diverges from the working raw-HAL sequence.
- Whether this is NCS-3.0.2-specific or affects current NCS too. We're
  locked to 3.0.2 here so didn't test.
- Worth filing a precise bug with Nordic. The two reproducer test apps
  I wrote (one using `spi_transceive`, one using raw HAL) differ only
  in the software layer — that's the kind of minimal repro that's
  hard to argue with. Happy to send over if you want.

Neither of those open items blocks production — the HAL workaround is
in `spi_probe.c` and works.
