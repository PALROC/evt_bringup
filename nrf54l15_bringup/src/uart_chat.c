/*
 * Copyright (c) 2026 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-MCU chat over nrf-sw-lpuart. See uart_chat.h for the API. This
 * implementation is identical on both MCUs — the only thing that differs
 * is which physical UART instance the `dut-uart` DT alias points at
 * (uart0 on the 9151, uart30 on the L15).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "uart_chat.h"

LOG_MODULE_REGISTER(uart_chat, LOG_LEVEL_INF);

static const struct device *const dut = DEVICE_DT_GET(DT_ALIAS(dut_uart));
static uart_chat_line_cb_t line_cb;

/* Assembled in the ISR, copied to `rx_pending` and dispatched on a
 * workqueue when a '\n' (or '\r') arrives. Single-slot — if a second
 * line completes before the workqueue runs, it overwrites the first.
 * That's fine for this demo's request/response cadence; would need a
 * proper k_msgq for high-rate traffic. */
static char rx_buf[UART_CHAT_LINE_MAX];
static size_t rx_idx;
static char rx_pending[UART_CHAT_LINE_MAX];

static void rx_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	if (line_cb) {
		line_cb(rx_pending);
	}
}
static K_WORK_DEFINE(rx_work, rx_work_handler);

/* All TX is deferred to the system workqueue: sw-lpuart's REQ/RDY
 * handshake completes via GPIOTE interrupts, and calling uart_poll_out
 * from any ISR blocks those interrupts and the send stalls partway. */
static char tx_msg[UART_CHAT_LINE_MAX];

static void tx_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	for (const char *p = tx_msg; *p; p++) {
		uart_poll_out(dut, *p);
	}
	uart_poll_out(dut, '\n');
}
static K_WORK_DEFINE(tx_work, tx_work_handler);

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}

	uint8_t c;
	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &c, 1) != 1) {
			continue;
		}
		if (c == '\r' || c == '\n') {
			if (rx_idx > 0) {
				rx_buf[rx_idx] = '\0';
				memcpy(rx_pending, rx_buf, rx_idx + 1);
				rx_idx = 0;
				k_work_submit(&rx_work);
			}
		} else if (rx_idx < UART_CHAT_LINE_MAX - 1) {
			rx_buf[rx_idx++] = (char)c;
		} else {
			/* Overflow — reset the buffer; this line is lost. */
			rx_idx = 0;
		}
	}
}

int uart_chat_init(uart_chat_line_cb_t on_line)
{
	if (!device_is_ready(dut)) {
		LOG_ERR("dut-uart device %s not ready", dut->name);
		return -ENODEV;
	}

	line_cb = on_line;
	uart_irq_callback_user_data_set(dut, uart_isr, NULL);
	uart_irq_rx_enable(dut);

	LOG_INF("uart_chat ready on %s (%s)", dut->name,
		on_line ? "line callback active" : "RX dropped (no cb)");
	return 0;
}

void uart_chat_send(const char *line)
{
	if (!line) {
		return;
	}
	strncpy(tx_msg, line, sizeof(tx_msg) - 1);
	tx_msg[sizeof(tx_msg) - 1] = '\0';
	LOG_INF("TX: \"%s\"", tx_msg);
	k_work_submit(&tx_work);
}

bool uart_chat_ready(void)
{
	return device_is_ready(dut);
}
