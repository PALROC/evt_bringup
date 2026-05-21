#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include "diag.h"

LOG_MODULE_REGISTER(diag, LOG_LEVEL_INF);

void diag_print_reset_reason(void)
{
	uint32_t cause = 0;
	int ret = hwinfo_get_reset_cause(&cause);

	if (ret) {
		LOG_WRN("hwinfo_get_reset_cause not supported: %d", ret);
		return;
	}

	/* Mirror via printk so the cause hits RTT immediately, even before the
	 * deferred LOG thread runs — survives RTT reconnects after a silent
	 * reset where the LOG queue might be drained slowly.
	 */
	printk("\n*** RESET CAUSE BITMAP: 0x%08x ***\n\n", cause);

	if (cause == 0) {
		LOG_INF("reset cause: 0 (cold start / cleared)");
	} else {
		LOG_INF("reset cause bitmap: 0x%08x", cause);
		if (cause & RESET_PIN)            LOG_INF("  - PIN reset");
		if (cause & RESET_SOFTWARE)       LOG_INF("  - SOFTWARE reset (sys_reboot/fault)");
		if (cause & RESET_BROWNOUT)       LOG_INF("  - BROWNOUT reset (supply dropped!)");
		if (cause & RESET_POR)            LOG_INF("  - POR (power-on reset)");
		if (cause & RESET_WATCHDOG)       LOG_INF("  - WATCHDOG reset");
		if (cause & RESET_DEBUG)          LOG_INF("  - DEBUG reset");
		if (cause & RESET_SECURITY)       LOG_INF("  - SECURITY reset");
		if (cause & RESET_LOW_POWER_WAKE) LOG_INF("  - LOW_POWER_WAKE");
		if (cause & RESET_CPU_LOCKUP)     LOG_INF("  - CPU LOCKUP");
		if (cause & RESET_PARITY)         LOG_INF("  - PARITY error");
		if (cause & RESET_PLL)            LOG_INF("  - PLL lock loss");
		if (cause & RESET_CLOCK)          LOG_INF("  - CLOCK error");
		if (cause & RESET_HARDWARE)       LOG_INF("  - generic HARDWARE");
		if (cause & RESET_USER)           LOG_INF("  - USER");
		if (cause & RESET_TEMPERATURE)    LOG_INF("  - TEMPERATURE");
	}

	/* Clear so the next reset's cause is independent. */
	hwinfo_clear_reset_cause();
}

/* Heartbeat: prove the kernel is still alive while main is blocked on
 * lte_lc_connect. If the heartbeats stop, something killed the system;
 * if they keep going past the silent point, main is just waiting.
 */
static void heartbeat_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (1) {
		LOG_INF("heartbeat @ %lld ms", k_uptime_get());
		k_sleep(K_SECONDS(2));
	}
}

K_THREAD_DEFINE(heartbeat_id, 1024, heartbeat_thread_fn,
		NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);
