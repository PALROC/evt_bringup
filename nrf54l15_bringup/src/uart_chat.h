/*
 * Copyright (c) 2026 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UART_CHAT_H
#define UART_CHAT_H

#include <stdbool.h>

/* Cross-MCU line-based chat over the nrf-sw-lpuart link (REQ/RDY GPIO
 * handshake). Same module on the 9151 and L15 sides; both boards expose
 * the `dut-uart` DT alias that the module binds to.
 *
 * Protocol: ASCII payload terminated by '\n'. Lines must fit in
 * UART_CHAT_LINE_MAX-1 bytes (the rest is the null terminator). No
 * framing on top — keep messages short and human-readable for easy RTT
 * debug. */

#define UART_CHAT_LINE_MAX 64

/* Callback fired once per received line. Runs on the system workqueue
 * thread (NOT in ISR context), so it's safe to do anything thread-side
 * here — log, set semaphores, call uart_chat_send() back, etc.
 *
 * The `line` pointer references an internal static buffer; copy out
 * what you need before the callback returns. */
typedef void (*uart_chat_line_cb_t)(const char *line);

/* Bind to the dut-uart device, hook up the RX interrupt + workqueue,
 * and register `on_line` for incoming traffic. Returns 0 on success,
 * negative errno otherwise. Pass NULL to disable line dispatch (RX
 * still happens, lines are just dropped). */
int uart_chat_init(uart_chat_line_cb_t on_line);

/* Queue a line for transmission. Adds a trailing '\n' automatically;
 * do NOT include one yourself. The actual TX runs on the system
 * workqueue (sw-lpuart TX must be in thread context, never ISR — see
 * the bring-up debug log section 5).
 *
 * Safe to call from any context (ISRs, work handlers, main thread).
 * Lines longer than UART_CHAT_LINE_MAX-1 are truncated. */
void uart_chat_send(const char *line);

/* True if the underlying uart device came up ready. */
bool uart_chat_ready(void);

#endif /* UART_CHAT_H */
