# How to Use RTT Logging on nRF54L15 DK

## Option 1: prj.conf changes (manual)

Add these lines to your `prj.conf`:

```conf
# Enable RTT
CONFIG_USE_SEGGER_RTT=y
CONFIG_RTT_CONSOLE=y
CONFIG_LOG_BACKEND_RTT=y

# Disable UART console
CONFIG_UART_CONSOLE=n
CONFIG_LOG_BACKEND_UART=n

CONFIG_LOG=y
CONFIG_CONSOLE=y
```

## Option 2: Build with the rtt-console snippet (cleanest way)

```bash
west build -b nrf54l15dk/nrf54l15/cpuapp --snippet rtt-console
west flash
```

This snippet sets everything automatically without touching `prj.conf`.

---

## Viewing RTT output

### J-Link RTT Viewer

1. Open **J-Link RTT Viewer** (from the J-Link Software pack)
2. Configure:
   - Connection: **USB**
   - Target Device: **nRF54L15_M33**
   - Target Interface: **SWD, 4000 kHz**
   - RTT Control Block: **Auto Detection**
3. Click OK

### nRF Connect for VS Code

Use the **RTT terminal** tab (next to the serial monitor) in the nRF Connect extension.

---

## Known gotchas on nRF54L15 DK

- You need **J-Link v8.14+** — older versions don't recognize `NRF54L15_M33` by name.
- RTT connection **breaks on reset** — reconnect in RTT Viewer after each flash/reset.
- Don't open RTT Viewer and the VS Code RTT terminal simultaneously — they conflict.
