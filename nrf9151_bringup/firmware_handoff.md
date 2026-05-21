# Firmware engineer handoff

This document is for the application firmware engineer picking up work after the
hardware bring-up phase. It is the practical companion to the formal bring-up
report (delivered separately as a PDF) and the full bring-up log (`steps.md`).

- **Read the LaTeX report first** for the high-level story and the validation tables.
- **Read `steps.md` § 16.9 and § 16.10** for the most recent technical findings.
- **Use this document** to know what to do next, what was tried, and what known-good
  baselines exist.

This is a working doc — extend it as you go.

**Freedom to refactor.** You're inheriting bring-up code, not application code. Feel
free to restructure it however suits your work — split modules, rename functions,
delete probe code you don't need, **rename the custom boards**, reorganise the
sysbuild layout, swap the board root layout, etc. None of the names or structures in
this repo are load-bearing. The only things to preserve are the behaviours documented
in the report and in this doc (correct DTS pin maps, the K32SRC RC fix, the LP-UART
fix path, etc.).

---

## Toolchain and first-time setup

All bring-up work was done with:

- **nRF Connect SDK 3.3.0** (Zephyr 4.3.99)
- **SEGGER J-Link V8.76** (host-side software)

If you've never added a custom board to nRF Connect for VS Code, the boards in this
repo (`boards/palroc/nrf9151dk` and `nrf54l15/boards/palroc/nrf54l15_palroc`) will not
be discovered by the build system until you tell the extension where to look:

1. Open VS Code → **Extensions** panel.
2. Search for **nRF Connect for VS Code**, click the gear icon → **Extension Settings**.
3. Find the **Board Roots** option and add the absolute path to the `boards/` directory
   (and to `nrf54l15/boards/` for the L15 build). Without this, the build will fail
   with "board not found" errors before it even starts compiling.

After that, the `west build` commands in the next section work normally.

---

## How to read this repo

| File | Purpose |
|---|---|
| `src/main.c` | nRF9151 application (currently stripped to "red LED on" — see commit `1497b72`) |
| `nrf54l15/src/main.c` | nRF54L15 application (BLE advertising + RFFE_BLE_WIFI_ENABLE drive) |
| `prj.conf` / `nrf54l15/prj.conf` | App Kconfig per chip |
| `sysbuild.conf` | Sysbuild-level Kconfig (nRF70 enablement lives here, not in `prj.conf`) |
| `boards/palroc/nrf9151dk/` | 9151-side board files |
| `nrf54l15/boards/palroc/nrf54l15_palroc/` | L15-side board files |
| `steps.md` | Full bring-up history (long; § 16.9 / 16.10 are the latest open items) |
| `test_summary.md` | Per-board RTT results + "Known validation gaps" |
| (formal report) | Delivered separately as a PDF — not in this repo |
| `how_to_rtt.md` | RTT setup notes (the L15 RTT block address is non-standard — see file) |

---

## Operational rules right now

Until you fix the cross-MCU contention (item 1 below):

1. **Erase one MCU's flash before testing the other.** Both **I2C2** and **both SPI
   buses** (SPI3 *and* SPI1/Wi-Fi) are shared between the 9151 and L15, and traffic
   on all three is corrupted whenever L15 firmware is running. Symptoms can look like
   "the PMIC driver is broken" or "Wi-Fi SPI is broken" — but the buses are just being
   trampled. To reset to a clean state: recover and erase the L15, then flash whichever
   side you want to test.
2. **Power supply: PPK2 source mode is now safe** on this board, after a stray green
   cable that was injecting voltage into the 3V3 rail was identified and cut. (Earlier
   notes in `steps.md` and `test_summary.md` claiming PPK2 source mode is unsafe and
   that boards must be powered from battery only are **stale** — the green-cable fix
   resolved that incident.)
3. **Known-good baselines.** Commit `4193f05` (`report`) is the last commit before the
   abandoned UART debugging work. Commit `1497b72` strips the 9151 to red-LED-only
   firmware — useful for testing the L15 in isolation without 9151 SPI/I2C activity.

---

## Open items, prioritized

### 1. Cross-MCU bus arbitration (blocker for any integrated testing)

**Problem.** Three buses are shared between the 9151 and L15, and **all three**
corrupt when both MCUs are running firmware:

- **I2C2** — SDA = 9151 P0.09 / L15 P1.04; SCL = 9151 P0.08 / L15 P1.05.
- **SPI3** — SCK/MOSI/MISO on 9151 P0.13/P0.15/P0.14.
- **SPI1 (Wi-Fi)** — used by the 9151 to talk to the nRF7000. **Also crashes when both
  MCUs are flashed**, even though the L15 is not the intended driver of this bus —
  the L15's GPIO subsystem is touching shared lines because the L15 DTS is not yet
  fully configured (see item 5).

The L15 firmware's `SYS_INIT` hook that parks P1.4/P1.5 as high-Z inputs was an
attempted workaround for I2C2 only — it is **not sufficient** even for I2C2, and does
not address SPI3 or SPI1 at all.

**What's needed.**

1. **Complete the L15 DTS first** (see item 5) so the L15 properly declares the
   shared SPI/I2C nodes and parks the shared pins. Until that's done, the only safe
   operational path is "recover and erase the L15 before running the 9151 side."
2. **Then design a software bus-arbitration protocol** between the two MCUs. The most
   likely shape: the 9151 is the bus master, and the L15 requests bus access via the
   inter-MCU LP-UART (item 2). Both MCUs running simultaneously is **not safe** until
   this protocol exists and is validated.

**v2 hardware delta (already locked in):** SPI3 will be split — the L15 gets its own
SPI bus, and LSM6DSO moves to I2C2. So in v2, only I2C2 will remain shared.

### 2. Inter-MCU UART link (the LP-UART) — this is the big one

**Goal.** A reliable byte-level link between the two MCUs over the `nrf-sw-lpuart`
driver (Nordic software low-power UART with REQ/RDY GPIO handshake). This is the
designated channel for the bus-arbitration protocol in item 1, so nothing else can
move forward until it works.

**Pin map (validated wiring).**

| Signal | nRF9151 pin | nRF54L15 pin |
|---|---|---|
| Data: 9151 → L15 | P0.03 (TX) | P0.02 (RX) |
| Data: L15 → 9151 | P0.02 (RX) | P0.00 (TX) |
| REQ (9151 requests transfer) | P0.31 | P1.11 (RDY input) |
| REQ (L15 requests transfer) | P0.30 (RDY input) | P1.12 |

**Backup pins:** the schematic also wires what would have been hardware-flow-control
**CTS/RTS** between the two MCUs — these are unused by `nrf-sw-lpuart` (REQ/RDY
replace them entirely), but they are physically connected. **If REQ/RDY misbehave for
any reason, you can move the LP-UART REQ/RDY assignment onto the CTS/RTS pin pair as
a fallback.** Check the schematic for the exact net names; on the 9151 side the
default CTS/RTS pinctrl was P0.07 / P0.06.

**What was tried, what happened, what's known.**

A full LP-UART setup was implemented in commit `b2a5001` (`uart debugging`) and then
**rolled back** because we couldn't get past one specific symptom — see "Status" below.
The commit is preserved on local reflog only and is not on any branch. The full diff
is captured in **Appendix A** of this document so you have it regardless of how the
repo was handed to you.

**Status when rolled back:** the REQ/RDY handshake works — both sides see each other
and the L15 arms its RX correctly. But the 9151's UARTE async TX never delivers any
data bytes on the wire, so the L15 fires `UART_RX_BUF_RELEASED` with `rx_got_data=false`
and logs `<err> lpuart: Empty receiver state:4`. So: the protocol fires, the wire is
silent.

**Root cause hypothesis (unverified — start here).** The `b2a5001` `prj.conf` uses
the **deprecated** approach for hardware-async UART:

```kconfig
CONFIG_UART_0_ASYNC=y
CONFIG_UART_0_INTERRUPT_DRIVEN=n
CONFIG_UART_0_NRF_HW_ASYNC=y
CONFIG_UART_0_NRF_HW_ASYNC_TIMER=2
CONFIG_UART_0_NRF_ASYNC_LOW_POWER=y
```

In NCS 3.3.0, `UART_0_NRF_HW_ASYNC` is deprecated. The correct pattern is to attach a
TIMER instance to the UART node via a DT phandle:

```dts
&uart0 {
    timer = <&timer2>;
    /* ...rest of node... */
};

&timer2 {
    status = "okay";
};
```

`timer2` is `status = "disabled"` by default in `nrf91_peripherals.dtsi` — you must
enable it. The UARTE driver reads the timer's register address via
`DT_REG_ADDR(DT_PHANDLE(UARTE(idx), timer))`. With the phandle in place, you can
remove the deprecated Kconfigs.

**Plan.**

1. Restore the LP-UART DTS from `b2a5001` (or apply Appendix A).
2. **Don't** copy the deprecated Kconfigs. Instead:
   - Add `timer = <&timer2>;` to the `&uart0` node in
     `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common.dtsi`.
   - Enable `&timer2` in the same file.
   - In `prj.conf`, keep `CONFIG_NRF_SW_LPUART=y`, `CONFIG_NRF_SW_LPUART_INT_DRIVEN=y`,
     `CONFIG_UART_0_ASYNC=y`, `CONFIG_UART_0_INTERRUPT_DRIVEN=n`. Drop the three
     `CONFIG_UART_0_NRF_HW_ASYNC*` lines.
3. Build and flash both sides. Send a known byte from the 9151 every second.
4. On the L15 RTT log, look for the byte arriving without the `Empty receiver state:4`
   error.

**Reference (DK test).** Oriol got the LP-UART pattern working end-to-end between
two Nordic DKs (not custom PCBs) on the same NCS version. **Ask Oriol for repo access
to the DK test build** — it is a known-good reference and will save you a lot of time
if the custom-board build refuses to come up.

### 3. nRF7000 Wi-Fi: 2.4 GHz scan and end-to-end RF path

**Problem.** Wi-Fi scan was only run from the 9151 with the BGS12SN6E6 RF switch's VC
line **manually wired to VCC** on the bench. Only **5 GHz APs** were visible —
2.4 GHz APs were absent.

**Production RF path.**

1. **L15 P1.10 (`RFFE_BLE_WIFI_ENABLE`) HIGH** → RV2C010UNT2L gate FET closes →
   RF switch VDD powered. *(L15 firmware already does this at boot — commit `59c496f`.)*
2. **`SW_CTRL0` (= `2.4G_SW_CTRL`)** drives the BGS12SN6E6's VC pin and is **driven
   automatically by the nRF7000** itself (it's an output from the Wi-Fi chip, not a
   GPIO that firmware needs to manage). No firmware action required for this signal.

**Plan.**

1. With L15 firmware running (`RFFE_BLE_WIFI_ENABLE` HIGH = RF switch powered) **and**
   the 9151 running the Wi-Fi scan, re-run the scan and verify both 2.4 GHz and 5 GHz
   APs are visible. The previous "5 GHz only" result was likely because the RF switch
   itself was unpowered (P1.10 was not driven during the bench test) — now that the
   L15 firmware drives P1.10 HIGH at boot and SW_CTRL0 is automatic from the nRF7000,
   the band selection should resolve itself.
2. Note: this is item 1's blocker — both MCUs need to run simultaneously, which
   requires bus arbitration (item 1) to be safe. Until then this test can only be run
   by accepting the I2C2/SPI3/SPI1 contention and verifying scan results survive it.

**Architectural note (v2 consideration, but worth flagging now).** Ideally, the
**nRF54L15 (BLE chip) should be the host of the nRF7000**, not the 9151. The 9151
is a cellular SoC and shouldn't be busy driving Wi-Fi scans; the L15 is a much more
natural fit for the radio coexistence side of the system. In v1 the SPI1 is wired
between the 9151 and nRF7000 — the FW engineer should plan the application
architecture with the v2 reassignment in mind, so any application-level Wi-Fi code
can be moved to the L15 with minimal churn when the hardware change lands.

### 4. GNSS — deferred to EVT2

The GNSS LNA was likely damaged when the LDO load-switch issue exposed it to 5 V VBUS.
**No replacement part is currently in hand**, and re-validation is deferred until EVT2
when a non-burned board is available for testing.

**Caveat on the pre-incident baseline.** Earlier docs (`steps.md` step 10,
`bringup_report_cody.tex`) report "12 SVs tracked, best CN0 41 dB-Hz outdoors" as a
GNSS validation baseline. Oriol now suspects that result was a **bug in the probe
code**, not a real fix — so do not treat 12 SVs / 41 dB-Hz as a known-good number to
match against. Treat GNSS as "fully unvalidated" on this platform until an EVT2 board
with a known-good LNA produces an independent result.

The LNA enable path itself (LDO1 → COEX0 level-shifted to match BUCK2) is correct
in firmware/DTS, and now runs at **3.3 V** (both BUCKs are pinned at 3.3 V — the
earlier 2.7 V level-shift mentioned in `steps.md` step 10 was from an initial test
when we thought the nRF7000 VBAT had to be above VDDIO; that constraint was reworked
and both rails now sit at 3.3 V). So the open work for the FW engineer when EVT2
lands is: flip `RUN_GNSS_PROBE=1`, confirm a clean tracking output (independent of
the suspect v1 baseline), and lock in a real reference number.

### 5. nRF54L15 board DTS is incomplete

The L15 DTS currently declares only `uart20` (still on DK default pins P1.4=TX /
P1.5=RX, which **conflict with the shared I2C2** — see the FIXME comment at the top
of `nrf54l15/boards/palroc/nrf54l15_palroc/nrf54l15_palroc-pinctrl.dtsi`). It does
**not** declare any SPI nodes, the LP-UART pinctrl, or the inter-MCU signal pins.
This contributes to the bus contention in item 1 — the L15's GPIO subsystem doesn't
know which pins are "shared, hands off."

**Plan.**

1. Move `uart20` off P1.4/P1.5 to free the shared I2C2 pins (or disable `uart20` if RTT
   is enough for logging — the firmware uses RTT, so `uart20` may not be needed at all).
2. Add the LP-UART pinctrl + `&uart30` node (Appendix A has the content).
3. Add the shared SPI3 node with `status = "disabled"` (or as an explicit
   high-Z park) so the GPIO subsystem treats those pins correctly.
4. Declare any other inter-MCU pins (`SW_CTRL0`, `RFFE_BLE_WIFI_ENABLE`, etc.) as named
   GPIOs in the DTS so firmware can `DT_NODELABEL` / `DT_ALIAS` them rather than
   hardcoding pin numbers in C (`main.c` currently hardcodes P1.10).

---

## Build instructions

### 9151 side

```sh
west build -b palroc/nrf9151dk/nrf9151_ns --sysbuild -p always
west flash
```

- **Sysbuild matters.** nRF70 Wi-Fi enablement is forced from `sysbuild.conf` via
  `SB_CONFIG_WIFI_NRF70` — setting `CONFIG_WIFI_NRF70` in `prj.conf` does **not** work
  in NCS 3.3.0 (sysbuild overrides it). See the comment block in `prj.conf`.
- **Modem firmware is already flashed** on the boards being handed over (Oriol flashed
  `mfw_nrf91x1` over SWD with the nRF Connect for Desktop programmer). You do not need
  to redo this. If you swap in a new bare 9151 chip later, note that it ships with PTI
  factory firmware only and the modem init will crash with `0xfff` / `PC=0` until
  you flash the modem firmware once.

### L15 side — two board variants are available

There are now **two L15 board variants** in `nrf54l15/boards/palroc/`. Pick whichever fits the work you're doing:

| Variant | Path | When to use |
|---|---|---|
| `nrf54l15_palroc` | `nrf54l15/boards/palroc/nrf54l15_palroc/` | The original bring-up board. Minimal DTS — only `uart20` (RTT/console) declared. BLE-validated. Use this for "is the L15 alive?" tests where you don't need any of the inter-MCU peripherals. |
| `nrf54l15_palroc_complete` | `nrf54l15/boards/palroc/nrf54l15_palroc_complete/` | Pin Planner-generated, with all peripherals declared (SPI00, SPI21, I2C20, UART30 + LP-UART scaffold) and named-GPIO aliases for every nRF7000 / RFFE / coex signal. **This is the variant Ralph should build on top of.** Already patched with the LFXO disable, the i2c20 disable (cross-MCU contention), the LP-UART scaffold (with placeholder REQ/RDY pins), and the HFXO `internal`/14000 fF override. |

**Original variant build:**
```sh
cd nrf54l15
west build -b nrf54l15_palroc/nrf54l15/cpuapp -p always
west flash
```

**Complete variant build:**
```sh
cd nrf54l15
west build -b nrf54l15_palroc_complete/nrf54l15/cpuapp -p always
west flash
```

**Before flashing the complete variant for any LP-UART work**, two things still need to happen:

1. **Confirm REQ/RDY pin numbers** in `nrf54l15_palroc_complete_common.dtsi` against the new schematic. Current placeholders are P1.12 (REQ = 44) and P1.11 (RDY = 43), inherited from the prior board. The CTS/RTS pair on P0.02 / P0.03 is left wired as a backup.
2. **Add LP-UART Kconfigs to `nrf54l15/prj.conf`**:
   ```kconfig
   CONFIG_SERIAL=y
   CONFIG_NRF_SW_LPUART=y
   CONFIG_NRF_SW_LPUART_INT_DRIVEN=y
   CONFIG_UART_INTERRUPT_DRIVEN=y
   CONFIG_UART_30_INTERRUPT_DRIVEN=n
   CONFIG_UART_30_ASYNC=y
   ```
   Without these, the lpuart child node is dead DTS — firmware will build but the link won't transmit.

**Known palroc HFXO gotcha** (already fixed in both variants — flagged here so you don't reintroduce it): the Nordic Pin Planner generator defaults `&hfxo { load-capacitors = "external"; }`, but palroc has no external XL1/XL2 caps. With `external`, BLE silently advertises off-channel and the device is invisible to phone scanners. The fix in both variants is `load-capacitors = "internal"` + `load-capacitance-femtofarad = <14000>`. If you ever regenerate a board variant from Pin Planner, override `&hfxo` immediately.

- **`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` is mandatory.** No 32 kHz crystal on this
  board; without RC selected, the kernel hangs at boot. Already in `prj.conf`.
- **RTT viewer auto-detect doesn't work on the L15.** Look up `_SEGGER_RTT` from
  `zephyr.elf` via `nm` and enter the address manually in the RTT viewer. See
  `how_to_rtt.md`.

---

## Useful commit hashes

| Commit | Purpose |
|---|---|
| `4193f05` | Last known-good before LP-UART debugging — clean baseline |
| `b2a5001` | LP-UART debugging (rolled back; reflog only — see Appendix A) |
| `1497b72` | 9151 firmware stripped to red-LED-only — use to isolate L15 testing |
| `59c496f` | L15 firmware drives `RFFE_BLE_WIFI_ENABLE` (P1.10) HIGH at boot |
| `c7cafb9` | Hall sensor (DRV5032FBDBZR) polarity finalised |
| `04d34bd` | Final MPPT/battery-protection resistor BOM |

---

## Appendix A — LP-UART DTS (from rolled-back commit `b2a5001`)

These are the exact DTS deltas to restore the LP-UART link. **Do not copy the
deprecated `prj.conf` Kconfigs from this commit** — use the DT `timer = <&timer2>`
phandle approach described in item 2 above.

### `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common-pinctrl.dtsi`

Replace the `uart0_default` / `uart0_sleep` blocks with:

```dts
uart0_default: uart0_default {
    group1 {
        psels = <NRF_PSEL(UART_TX, 0, 3)>;
    };
    group2 {
        psels = <NRF_PSEL(UART_RX, 0, 2)>;
        bias-pull-up;
    };
};

uart0_sleep: uart0_sleep {
    group1 {
        psels = <NRF_PSEL(UART_TX, 0, 3)>,
                <NRF_PSEL(UART_RX, 0, 2)>;
        low-power-enable;
    };
};
```

### `boards/palroc/nrf9151dk/nrf9151dk_nrf9151_common.dtsi`

Add to `aliases`:
```dts
inter-mcu-uart = &lpuart;
```

Replace the `&uart0` node and add the `&gpiote` interrupt override:

```dts
&uart0 {
    status = "okay";
    current-speed = <115200>;
    pinctrl-0 = <&uart0_default>;
    pinctrl-1 = <&uart0_sleep>;
    pinctrl-names = "default", "sleep";
    /delete-property/ hw-flow-control;

    /* ADD per fix recommendation in firmware_handoff.md item 2: */
    timer = <&timer2>;

    lpuart: nrf-sw-lpuart {
        compatible = "nordic,nrf-sw-lpuart";
        status = "okay";
        req-pin = <31>;   /* P0.31 — 9151 requests; L15 monitors as RDY */
        rdy-pin = <30>;   /* P0.30 — 9151 receives L15 REQ */
    };
};

&gpiote {
    status = "okay";
    interrupts = <49 NRF_DEFAULT_IRQ_PRIORITY>;
};

/* ADD per fix recommendation: */
&timer2 {
    status = "okay";
};
```

### `nrf54l15/boards/palroc/nrf54l15_palroc/nrf54l15_palroc-pinctrl.dtsi`

Add at the top of the `&pinctrl` block:

```dts
/omit-if-no-ref/ uart30_default: uart30_default {
    group1 { psels = <NRF_PSEL(UART_TX, 0, 0)>; };
    group2 {
        psels = <NRF_PSEL(UART_RX, 0, 2)>;
        bias-pull-up;
    };
};

/omit-if-no-ref/ uart30_sleep: uart30_sleep {
    group1 {
        psels = <NRF_PSEL(UART_TX, 0, 0)>,
                <NRF_PSEL(UART_RX, 0, 2)>;
        low-power-enable;
    };
};
```

### `nrf54l15/boards/palroc/nrf54l15_palroc/nrf54l15_palroc_cpuapp_common.dtsi`

Add to `aliases`:
```dts
inter-mcu-uart = &lpuart;
```

Add the `&uart30` node:

```dts
&uart30 {
    status = "okay";
    current-speed = <115200>;
    pinctrl-0 = <&uart30_default>;
    pinctrl-1 = <&uart30_sleep>;
    pinctrl-names = "default", "sleep";
    /delete-property/ hw-flow-control;

    lpuart: nrf-sw-lpuart {
        compatible = "nordic,nrf-sw-lpuart";
        status = "okay";
        req-pin = <44>;   /* P1.12 */
        rdy-pin = <43>;   /* P1.11 */
    };
};
```

### `nrf54l15/prj.conf`

Add (these are fine to copy verbatim — none of these are deprecated):

```kconfig
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_NRF_SW_LPUART=y
CONFIG_NRF_SW_LPUART_INT_DRIVEN=y
CONFIG_UART_30_INTERRUPT_DRIVEN=n
CONFIG_UART_30_ASYNC=y
```

### `prj.conf` (9151 side)

Add (note: omit the deprecated `UART_0_NRF_HW_ASYNC*` lines from `b2a5001`):

```kconfig
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_NRF_SW_LPUART=y
CONFIG_NRF_SW_LPUART_INT_DRIVEN=y
CONFIG_NRF_SW_LPUART_MAX_PACKET_SIZE=64
CONFIG_UART_0_ASYNC=y
CONFIG_UART_0_INTERRUPT_DRIVEN=n
```
