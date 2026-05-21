# nrf9151_bringup

nRF9151 bring-up firmware. Symmetric nrf-sw-lpuart ping-pong test against
the L15 — sends `PING\n` every 5 s and replies `PING RECEIVED\n` on
incoming PING.

## Build

Requires the `palroc_nova` boards repo on disk (sibling of this repo's
parent), exposed via `BOARD_ROOT`. NCS v3.0.2.

```bash
# EVT1
west build -b nrf9151_nova_evt1/nrf9151/ns -d build_evt1 -p \
           -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
west flash -d build_evt1

# EVT2 (when nrf9151_nova_evt2 lands in palroc_nova)
west build -b nrf9151_nova_evt2/nrf9151/ns -d build_evt2 -p \
           -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
```

## RTT viewer

On `/ns` builds the `_SEGGER_RTT` block lands above TF-M's secure RAM
region, outside J-Link RTT Viewer's default auto-detect window. Either
widen the search range (start `0x20000000`, size `0x40000`) or grab the
exact address from the build's map file:

```bash
grep _SEGGER_RTT build_evt1/nrf9151_bringup/zephyr/zephyr.map
```
