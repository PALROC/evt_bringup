# CoAP heartbeat server — intern brief

## What you're building

A small Python server that runs on our VPS (**158.179.212.244**) and
listens for CoAP heartbeats from our IoT devices. Each heartbeat is a
small CBOR-encoded message with telemetry (uptime, battery, signal,
etc.). Your job is to receive them, decode them, and log them so we
can dashboard which devices stayed alive overnight.

## Stack

- **Python 3.10+**
- **`aiocoap`** for the CoAP server
- **`cbor2`** for payload decoding
- That's it. No DB, no web framework yet — just stdout logs + a CSV
  file.

## What the protocol looks like

- Transport: plain UDP CoAP (no DTLS in v1)
- Port: **5683** (the IANA-assigned CoAP port)
- Resource path: **`/hb`**
- Method: **POST**
- Content-Format: `application/cbor` (option 60, value 0x3C)
- Server should reply with a **2.04 Changed** ACK so the device knows
  it was received.

## The payload — a CBOR map with these keys

| Key    | CBOR type   | Meaning |
|--------|-------------|---------|
| `id`   | text string | 16-char hex chip ID (e.g. `"1b62399045bf9c64"`) |
| `up`   | uint        | seconds since the device booted |
| `boot` | uint        | boot counter on the device |
| `vbat` | int         | battery voltage in millivolts (e.g. 3789 = 3.789 V) |
| `ibat` | int         | battery current in milliamps, **signed**: positive = charging (solar > load), negative = discharging |
| `ntc`  | int         | battery NTC temperature in °C |
| `rsrp` | int         | cellular signal strength in dBm (negative number, e.g. -101) |
| `rat`  | text string | radio access tech: `"LTE-M"`, `"NB-IoT"`, or `""` |
| `mC`   | int         | modem die temperature in °C |
| `ax`   | int         | accelerometer X, milli-g (1000 ≈ 1 g) |
| `ay`   | int         | accelerometer Y, milli-g |
| `az`   | int         | accelerometer Z, milli-g |
| `iC`   | int         | IMU die temperature in °C |
| `iok`  | uint        | 1 = IMU read succeeded; 0 = I²C error / IMU absent |
| `rst`  | uint        | reset reason enum: 1=power-on, 2=brownout, 3=pin, 4=software, 5=watchdog, 6=low-power-wake, 7=other, 0=unknown |
| `fw`   | text string | firmware version string |

Around 160-190 bytes per heartbeat.

> **Schema versioning note:** older device builds may send fewer keys
> (no `ibat`/`ntc`/`mC`). Use `payload.get(key, default)` everywhere
> so the decoder doesn't crash on older firmware.

## What you need to do

### 1. Receive + ACK

Spin up `aiocoap` listening on UDP `0.0.0.0:5683`, register a `Resource`
at the path `hb` that accepts POST. On every request:
- decode the payload with `cbor2.loads(request.payload)`
- print a one-line summary to stdout (timestamp + chip ID + uptime + RSRP)
- write a row to `heartbeats.csv` (see schema below)
- return `aiocoap.Message(code=aiocoap.CHANGED)` so the device gets a 2.04 ACK

### 2. CSV schema

One file `heartbeats.csv`, append-mode, with header:

```
server_ts_utc,chip_id,uptime_s,boot_count,vbat_mV,ibat_mA,ntc_C,rsrp_dBm,rat,modem_C,accel_x_mg,accel_y_mg,accel_z_mg,imu_C,imu_ok,reset_reason,fw_version,client_ip
```

`server_ts_utc` = ISO 8601 UTC timestamp of when the server received the
message. `client_ip` = source IP of the UDP packet (useful for debugging
cellular roaming).

Just `csv.writer.writerow([...])` is fine; no fancy formatting needed.

### 3. "Who's alive" dashboard (CLI tool)

A small companion script `status.py` that:
- Reads `heartbeats.csv`
- For each unique `chip_id`, finds the most recent heartbeat
- Prints a colored table:
  - **Green** if last heartbeat was within 2× expected interval (so under 10 min, since the device sends every 5 min)
  - **Yellow** if 2-6× interval (10-30 min — possibly recovering from a network blip)
  - **Red** if older than 6× interval (likely actually offline)

Columns: chip_id, last_seen (relative time like "3m ago"), uptime, vbat, rsrp, rat, fw_version.

This script just runs once and prints — no daemon needed. Run it with
`python status.py` whenever someone asks "are the boards still alive?".

### 4. Systemd unit so it survives reboots

Drop a `coap_heartbeat.service` file in `/etc/systemd/system/` that
runs the server, restarts on crash, captures stdout to journald.
Standard pattern, nothing fancy.

### 5. Firewall

On the VPS, open UDP 5683:
```
sudo ufw allow 5683/udp
```

## Repository layout to aim for

```
coap_heartbeat_server/
├── README.md               <- how to run it
├── server.py               <- the aiocoap listener (main entry point)
├── status.py               <- the CLI dashboard
├── requirements.txt        <- aiocoap, cbor2
├── coap_heartbeat.service  <- systemd unit
└── heartbeats.csv          <- generated at runtime; .gitignore it
```

## Smoke testing

Once it's running on the VPS, you can verify it with this from your
laptop (assuming Python + aiocoap installed locally):

```python
# test_post.py
import asyncio, cbor2, aiocoap

async def main():
    payload = cbor2.dumps({
        "id": "deadbeef12345678", "up": 1, "boot": 1,
        "vbat": 3800, "ibat": 45, "ntc": 27,
        "rsrp": -85, "rat": "LTE-M", "mC": 32,
        "ax": 12, "ay": -8, "az": 998, "iC": 26, "iok": 1,
        "rst": 1,
        "fw": "test-0.0.1",
    })
    ctx = await aiocoap.Context.create_client_context()
    req = aiocoap.Message(code=aiocoap.POST,
                          uri="coap://158.179.212.244/hb",
                          payload=payload,
                          content_format=60)
    r = await ctx.request(req).response
    print(f"got: {r.code}")
asyncio.run(main())
```

Should print `got: 2.04 Changed`, and the heartbeat should appear in
the CSV + the dashboard.

## Security hardening (REQUIRED for v1)

The server is sitting on a public UDP port — anyone scanning the
internet will find it. We're not adding DTLS yet, so we mitigate at
the application layer. These are non-negotiable for v1:

### 1. Chip-ID allowlist

We only have 5 boards. Anything reporting a different chip ID is
either spoofed or a misconfigured device. Reject it.

```python
# Top of server.py — update when new boards are validated
ALLOWED_CHIP_IDS = {
    "1b62399045bf9c64",   # evt2_brd1
    "b0844c400a825942",   # evt2_brd2
    "9c4ff37f0ae95bc1",   # evt2_brd3
    "efcc0eb933bc2550",   # evt2_brd4
    "9cb8ea24bb7b8f12",   # evt2_brd5
}

# In your POST handler, after decoding the CBOR:
if data.get("id") not in ALLOWED_CHIP_IDS:
    # Log the source IP + the rogue chip_id to a separate file so we
    # can review what's hitting the port, but don't log it to the main
    # heartbeats.csv.
    log_rogue(request.remote.hostinfo[0], data.get("id"))
    return aiocoap.Message(code=aiocoap.FORBIDDEN)
```

### 2. Payload size cap

CBOR decoders are a known DoS vector — a small malicious payload can
expand into gigabytes of memory. Cap input length BEFORE you call
`cbor2.loads`.

```python
MAX_PAYLOAD_BYTES = 512  # real heartbeats are ~100 bytes

if len(request.payload) > MAX_PAYLOAD_BYTES:
    return aiocoap.Message(code=aiocoap.REQUEST_ENTITY_TOO_LARGE)
```

### 3. Wrap the CBOR decode in try/except

Any decode error should respond `4.00 Bad Request`, NOT crash the
Python process. systemd will restart it but flapping is bad.

```python
try:
    data = cbor2.loads(request.payload)
    if not isinstance(data, dict):
        raise ValueError("payload is not a CBOR map")
except Exception as e:
    # log source IP + first 32 hex bytes of payload for triage
    log_bad_payload(request.remote.hostinfo[0], request.payload[:32])
    return aiocoap.Message(code=aiocoap.BAD_REQUEST)
```

### 4. Per-source-IP rate limit

A flood of valid-but-spoofed packets from one source could fill the
disk fast. Soft limit: drop heartbeats from any one source IP
that's sending more than 2 packets per second.

```python
# Simple sliding-window dict, no external library needed
from collections import deque
import time

RECENT = {}                          # ip -> deque of recent timestamps
RATE_LIMIT_WINDOW_S = 1.0
RATE_LIMIT_MAX_REQS = 2

def rate_limit_ok(ip):
    now = time.monotonic()
    dq = RECENT.setdefault(ip, deque())
    while dq and dq[0] < now - RATE_LIMIT_WINDOW_S:
        dq.popleft()
    if len(dq) >= RATE_LIMIT_MAX_REQS:
        return False
    dq.append(now)
    return True

# In the POST handler:
ip = request.remote.hostinfo[0]
if not rate_limit_ok(ip):
    return aiocoap.Message(code=aiocoap.SERVICE_UNAVAILABLE)
```

### 5. Run as an unprivileged user

The systemd unit MUST NOT run the server as root. Create a dedicated
service account with no shell, no home dir, no sudo:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin coap
sudo mkdir -p /var/lib/coap_heartbeat
sudo chown coap:coap /var/lib/coap_heartbeat
```

The systemd unit (`/etc/systemd/system/coap_heartbeat.service`) should
have these directives — they sandbox the service so even if the
Python process is compromised, the attacker can't pivot out:

```ini
[Service]
User=coap
Group=coap
WorkingDirectory=/var/lib/coap_heartbeat
ExecStart=/usr/bin/python3 /opt/coap_heartbeat/server.py

# Sandboxing — turns on if available, fails gracefully otherwise.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/coap_heartbeat
RestrictAddressFamilies=AF_INET AF_INET6
RestrictNamespaces=true
LockPersonality=true
MemoryDenyWriteExecute=true

# Reasonable resource limits — kills the process if it goes runaway.
LimitNOFILE=256
MemoryMax=128M

Restart=on-failure
RestartSec=5s
```

The `ReadWritePaths` line is crucial — it's the ONLY directory the
service can write to. Everything else on the filesystem is read-only
from the service's perspective.

### 6. Firewall scope

`sudo ufw allow 5683/udp` opens ONLY that port. Don't open more.
Specifically don't open TCP 5683 (CoAP-over-TCP, which we don't
use) — leaving it closed reduces attack surface.

### What this gets us

After applying 1-6, the realistic risk is:
- Worst case: Someone floods the port → rate limiter kicks in →
  legitimate traffic still gets through, no crash, no disk fill.
- Someone sends malformed CBOR → bad request response, process keeps
  running.
- Someone sends valid heartbeats with a spoofed chip ID → rejected,
  logged to a separate "rogue" file we can review later.
- Even in the (very unlikely) case of a Python RCE bug, the service
  is sandboxed to one directory and one network family.

Production-grade security comes when we add DTLS-CID with PSKs in
v3. For now, this is acceptable for an internal reliability test.

---

## Things NOT in scope for v1

(So you don't get sidetracked.)

- **No DTLS / authentication** — pretend the public CoAP port is fine
  for our test phase. We'll add DTLS-CID later when the protocol is
  proven and we want production-grade security.
- **No web UI** — the CLI dashboard is enough for now. If we want a
  proper UI later it'd be a separate task.
- **No alerting / Slack / email** — humans run `status.py` for now.
- **No database** — CSV is fine for thousands of heartbeats per board.
  We'll switch to SQLite or InfluxDB later if needed.

## Time estimate

About **half a day** if you've done Python network programming before.
A bit more if `aiocoap` is new — its docs are decent and the
`server.py` example in their repo is a great starting point.

## Questions to ping me on

- aiocoap import issues / Python version mismatch
- Anything weird with the CBOR decoding
- "Should I add X feature?" → almost always NO for v1, ask first

## Reference: device-side code

The firmware that posts heartbeats lives in
`evt_bringup/reliability_test/` in this repo. Specifically:
- `src/telemetry.c` defines the exact CBOR map keys (mirror these
  in the Python decoder)
- `src/coap_client.c` defines the wire-level CoAP request shape

If anything's ambiguous, that's the source of truth — the device emits
what it emits, and the server has to match.
