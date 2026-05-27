/*
 * nRF9151 DK SILENT firmware — just boots and sleeps.
 *
 * Use case: we want the 9151 DK powered + running normally (so any
 * weirdness from --eraseall is avoided) but absolutely NOT touching
 * the shared SPI3 bus. With this app flashed, the L15 DK can drive
 * the GD25 (via the P1.10 → P0.20 jumper) without any contention
 * from the 9151 side.
 *
 * Compared to `nrfjprog --eraseall`:
 *   - Erased: CPU is in a fault loop, hardware in undefined startup
 *             state, behavior is not "normal."
 *   - Silent: full Zephyr + TF-M boot, all peripherals at their
 *             post-init defaults, just main() doing nothing. Much
 *             cleaner state to reason about.
 *
 * Build:
 *   west build -p -b nrf9151dk/nrf9151/ns -d build
 *   west flash -d build --snr <9151_DK_serial>
 */

#include <zephyr/kernel.h>

int main(void)
{
	printk("\n");
	printk(">>> BUILD-TAG: dk_pair 9151dk_silent 2026-05-26\n");
	printk(">>> 9151 DK SILENT — Zephyr is up but no SPI driver is\n");
	printk(">>> compiled in. SPI3 stays untouched, all GPIOs default\n");
	printk(">>> to high-Z input. The L15 has the bus to itself.\n");

	uint32_t tick = 0;
	while (1) {
		k_msleep(10000);
		tick++;
		/* Heartbeat once per 10 s, just to confirm the 9151 is alive
		 * and not stuck in a fault. */
		printk(">>> 9151 still silent (t=%u s)\n", tick * 10);
	}

	return 0;
}
