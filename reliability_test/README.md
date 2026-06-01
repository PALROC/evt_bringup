# Reliability test — periodic CoAP heartbeats over LTE-M

Standalone app that wakes every N minutes, attaches LTE-M, POSTs a
small CBOR telemetry payload to a CoAP server on your VPS, then goes
back to PSM sleep. The server logs the heartbeats with per-device
chip IDs so we can see which boards stayed alive over long runs.

## What's in the heartbeat

CBOR map keyed by short strings:

| Key    | Type | Meaning |
|--------|------|---------|
| `id`   | str  | 8-byte hwinfo chip ID rendered as 16 hex chars |
| `up`   | uint | seconds since boot |
| `boot` | uint | boot counter (RAM-resident; survives soft reset only) |
| `vbat` | int  | battery voltage in mV (0 in v1 — PMIC hookup pending) |
| `rsrp` | int  | serving-cell RSRP in dBm |
| `rat`  | str  | "LTE-M" / "NB-IoT" / "" |
| `heap` | uint | free heap bytes (0 in v1) |
| `rst`  | uint | reset reason (1=POR 2=brownout 3=pin 4=soft 5=wdt …) |
| `fw`   | str  | firmware version |

~80-100 bytes encoded CBOR, ~150-180 bytes over the wire (CoAP+UDP+IP).

## Build + flash

Pristine build for an EVT2 board:

```bash
cd evt_bringup/reliability_test
west build -p -b nrf9151_nova_evt2/nrf9151/ns -d build \
    -- -DBOARD_ROOT=/home/oriol/PALROC_dev/NMS/phase2-hw/palroc_nova
west flash -d build --snr <board_serial>
```

For a stock 9151 DK (useful for first-stage testing without an EVT2):

```bash
west build -p -b nrf9151dk/nrf9151/ns -d build
```

## Before flashing — fill in `src/server_config.h`

```c
#define COAP_SERVER_HOST           "your-vps.example.com"   /* or IPv4 */
#define COAP_SERVER_PORT           5683
#define COAP_HEARTBEAT_PATH        "hb"
#define COAP_HEARTBEAT_INTERVAL_S  300                       /* 5 min */
```

The server-side Python listener (separate repo / folder, TBD) listens
on UDP port 5683 for POST/PUT requests to the `/hb` path and writes
each decoded heartbeat to a CSV.

## Expected RTT output

```
RELIABILITY TEST — periodic CoAP heartbeats
  server: your-vps.example.com:5683  path=/hb  interval=300 s
telemetry: chip_id=1b62399045bf9c64 reset_reason=1 fw=rel-test-0.1.0
telemetry: boot_count=1 (this session)
requesting LTE-M attach...
LTE registered (status=1)
resolved your-vps.example.com -> 203.0.113.42:5683
coap_client UDP socket ready (fd=0)
--- heartbeat #1 ---
  uptime=12s  vbat=0mV  rsrp=-88dBm  rat=LTE-M
  cbor payload 84 bytes
CoAP POST sent (96 bytes; payload 84)
CoAP response code 2.04 (OK)
heartbeat #1 OK — sleeping 300 s
--- heartbeat #2 ---
...
```

## What this proves over time

- **Cold-boot reattach reliability** (each reset reason is captured)
- **LTE-M long-duration stability** (does it drop the cell after N hours?)
- **CoAP / UDP / DTLS-CID transport** (v1 is plain UDP; DTLS in v2)
- **Modem PSM behaviour** (deep sleep between heartbeats works?)
- **Server-side per-device dashboard** of who's up vs who's gone silent

## Roadmap

- **v1 (this)**: plain CoAP, 5-min cadence, fixed VPS, basic telemetry
- **v2**: hook in real VBAT from the nPM1300, optionally add GNSS fix
  payload, add NVS-backed boot counter so it survives power loss
- **v3**: DTLS-CID transport (matches what nRF Cloud A-GNSS uses), so
  the link is encrypted + authenticated end-to-end
- **v4**: optional offline queue if heartbeats fail repeatedly — keep
  N hours of heartbeats in flash and replay when the link returns
