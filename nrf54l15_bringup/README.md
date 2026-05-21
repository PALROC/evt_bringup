# nrf54l15_bringup

nRF54L15 bring-up firmware. Symmetric nrf-sw-lpuart ping-pong test against
the 9151 — sends `PING\n` every 5 s and replies `PING RECEIVED\n` on
incoming PING.

## Build

Requires the `palroc_nova` boards repo on disk (sibling of this repo's
parent), exposed via `BOARD_ROOT`. NCS v3.0.2.

```bash
# EVT1, /ns (TF-M, production target)
west build -b nrf54l15_nova_evt1/nrf54l15/cpuapp/ns -d build_evt1 -p \
           -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
west flash -d build_evt1

# EVT2 (when nrf54l15_nova_evt2 lands in palroc_nova)
west build -b nrf54l15_nova_evt2/nrf54l15/cpuapp/ns -d build_evt2 -p \
           -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
```

## Why `prj.conf` carries explicit clock + RTT overrides

The L15 nova board has no 32 kHz crystal on XL1/XL2 — the LFXO is
disabled in the board DT, and the LFCLK runs off the internal RC.
Some upstream `_ns_defconfig` precedence makes the board's
`K32SRC_RC=y` default get shadowed back to `XTAL`, leaving the LFCLK
unsourced. Symptom: timestamps stuck at `00:00:00` and the scheduler
frozen, even though UART RX ISRs still log "RX: PING" (UARTE runs off
HFCLK).

`prj.conf` pins both halves of the K32SRC choice explicitly at app
level (`_RC=y`, `_XTAL=n`), plus the RTT console overrides that also
got shadowed. Do not delete them.

## RTT viewer

On `/ns` builds the `_SEGGER_RTT` control block lands above TF-M's
secure RAM region, outside J-Link RTT Viewer's default auto-detect
window. Quickest fix: in RTT Viewer connection dialog, set
"RTT Control Block" → "Address" → **`0x20013410`** (known-good from
prior bring-up builds; will be within a few hundred bytes of this for
any L15 /ns build).

For more robust auto-detect across rebuilds, widen the search range
instead: start `0x20013000`, size `0x1000` (or `0x40000` to cover the
full SRAM).

If RTT Viewer connects but shows no output, the exact address has
shifted — grep the current build's map file:

```bash
grep _SEGGER_RTT build_evt1/nrf54l15_bringup/zephyr/zephyr.map
```
