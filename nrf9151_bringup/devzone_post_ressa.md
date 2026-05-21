Hi Ressa,

A few things are still hurting me and I'd appreciate your input.

## 1. SPI3 / IMU damage

Apart from having both the nRF54L15 and the nRF9151 sitting on the same SPI3 bus as masters (which I know I need to fix on the next revision), I'm seeing a separate issue I can't explain on my own.

When the board is powered from the battery, everything works fine and the BUCK rails read the expected voltages. But the moment I plug VBUS (5 V), the nRF54L15 becomes unavailable for programming. While debugging this, I noticed that the BUCK that should be 3.3 V is now sitting at **4.2 V**. I have no explanation for that yet.

Could you take a look at the schematic and tell me if anything jumps out? Could it be related to the MPPT path somehow? I'm out of ideas.

## 2. Hall sensor + SHPHLD design

I wanted to use the same hall sensor as a regular input *and* as the way to keep SHPHLD pulled high so the device has a clean low-power shipping mode. But something in my design is wrong: with no magnet present, the nPM1300 keeps resetting every ~10 s.

I desoldered **Q4 (DMN2990UDJ-7)** and the device now runs without those resets, but it's clearly not the intended solution. I can fall back to a plain hall-only input (no shipping-mode tie-in), but if you have any recommendation for how to properly combine a hall sensor with SHPHLD on the nPM1300, I'd really appreciate it.

Thanks!

---

## Update — root cause found (2026-05-04)

We figured out the 4.2 V mystery without needing the schematic review, posting here in case it helps someone else.

**Root cause: LDO VIN = VSYS = 5 V when VBUS is connected, and the LDOs were wired as power switches.**

The LDOs on this board have their VIN tied to VSYS. On battery power VSYS ≈ 3.7–4.2 V, but once VBUS (5 V USB) is plugged in, VSYS rises to 5 V. The LDOs were originally designed to act as load switches (output tracks input), so the moment VBUS arrived, their outputs also jumped to 5 V. That 5 V leaked into the 3.3 V rail — which is why we were reading ≈4.2 V instead of 3.3 V. (The exact value depends on how far the rail had been pulled down by the loads on it.)

A separate wiring error — shorting LNA2 to the switch supply voltage — compounded the damage. That node sits at the LDO output, so it was also at 5 V.

**Damage assessment:**
- Most components on the 3.3 V rail appear to have survived (the nRF54L15 is still alive, which is lucky).
- The GNSS LNA is almost certainly damaged — its absolute maximum VCC is 3.4 V and it was exposed to 5 V.
- Any other parts on that rail with Vmax < 5 V should be treated as suspect.

**Fix for v2:** Connect LDOIN to the already-regulated 3.3 V rail instead of VSYS. That caps the LDO output at 3.3 V regardless of VBUS, and eliminates the 5 V leakage path entirely.
