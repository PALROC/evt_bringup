# evt_bringup

Per-MCU bring-up firmware for the PALROC Nova EVT boards (EVT1 today,
EVT2 next). Single git repo covering both MCUs so cross-MCU changes
(e.g., wire-protocol tweaks that touch both sides) can land in one
atomic commit.

## Layout

| Path | Contents |
|------|----------|
| `nrf9151_bringup/` | nRF9151 firmware. Builds against `nrf9151_nova_evtN` boards. |
| `nrf54l15_bringup/` | nRF54L15 firmware. Builds against `nrf54l15_nova_evtN` boards. |

The shared board definitions live in the [palroc_nova](https://github.com/PALROC/palroc_nova_boards)
repo, pinned to **nRF Connect SDK v3.0.2**.

## EVT versioning is board-level, not app-level

EVT1 and EVT2 differ only in hardware (pin reassignments, IMU bus,
sensor population). All those deltas live in the board files in
`palroc_nova/`; the app code in `evt_bringup/` is shared across EVT
versions. To build for a different EVT, just swap the board target:

```bash
# 9151 EVT1
west build -b nrf9151_nova_evt1/nrf9151/ns -d build_evt1 ...

# 9151 EVT2
west build -b nrf9151_nova_evt2/nrf9151/ns -d build_evt2 ...
```

Where the app has to branch on hardware presence (e.g., a sensor that
exists on EVT1 but not EVT2), it does so via DT-driven conditionals:

```c
#if DT_NODE_HAS_STATUS(DT_NODELABEL(hall1), okay)
    // EVT1-only path
#endif
```

No copy-paste forks per EVT, no drifting.

## Today's baseline

Both subprojects start with the same code: a symmetric nrf-sw-lpuart
ping-pong test (each side sends `PING\n` every 5 s and replies
`PING RECEIVED\n` on incoming PINGs). Validated end-to-end on EVT1
hardware, both DK and EVT1 PCB.

## Building

Each subproject's README has its exact build commands. Common requirement:

```bash
export BOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
```

or pass `-DBOARD_ROOT=...` on each `west build` invocation.
