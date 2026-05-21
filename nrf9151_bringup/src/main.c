/*
 * Symmetric inter-MCU link test over nrf-sw-lpuart.
 *
 * Behavior on every node (same code on both boards):
 *   - Every 5 seconds, send "PING\n".
 *   - On receiving "PING\n",          reply "PING RECEIVED\n".
 *   - On receiving "PING RECEIVED\n", just log (round-trip ack confirmed).
 *
 * If LED0 / LED1 are defined for the board, they blink on activity:
 *   LED0 toggles every time we get an ACK back (round-trip success).
 *   LED1 toggles every time we receive a PING (peer-is-alive indicator).
 *
 * EVT1 hardware reality: only the 9151 has connected LEDs. The L15 side
 * runs with no LEDs and reports only via RTT. The 9151's LED toggling
 * on every "PING RECEIVED" coming back is the acceptance test for the
 * inter-MCU link.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uart_chat, LOG_LEVEL_INF);

#define PING_INTERVAL K_SECONDS(5)

static const struct device *const dut_uart = DEVICE_DT_GET(DT_ALIAS(dut_uart));

#if DT_NODE_EXISTS(DT_ALIAS(led0))
#define HAS_LED0 1
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#else
#define HAS_LED0 0
#endif

#if DT_NODE_EXISTS(DT_ALIAS(led1))
#define HAS_LED1 1
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#else
#define HAS_LED1 0
#endif

#define RX_BUF_SZ 64
static char rx_buf[RX_BUF_SZ];
static size_t rx_idx;
static atomic_t rx_byte_count;

/* All TX is deferred to the system workqueue: nrf-sw-lpuart's REQ/RDY
 * handshake completes via GPIOTE interrupts, and calling uart_poll_out
 * from any ISR (RX callback, timer ISR) blocks those interrupts and the
 * send stalls partway. */

static void uart_send(const char *s)
{
	while (*s) {
		uart_poll_out(dut_uart, *s++);
	}
}

static void send_ping_work_handler(struct k_work *work)
{
	LOG_INF("TX: \"PING\"");
	uart_send("PING\n");
}
static K_WORK_DEFINE(send_ping_work, send_ping_work_handler);

static void send_ack_work_handler(struct k_work *work)
{
	LOG_INF("TX: \"PING RECEIVED\"");
	uart_send("PING RECEIVED\n");
}
static K_WORK_DEFINE(send_ack_work, send_ack_work_handler);

static void process_line(const char *line)
{
	LOG_INF("RX: \"%s\"", line);

	if (strcmp(line, "PING") == 0) {
#if HAS_LED1
		gpio_pin_toggle_dt(&led1);
#endif
		k_work_submit(&send_ack_work);
	} else if (strcmp(line, "PING RECEIVED") == 0) {
#if HAS_LED0
		gpio_pin_toggle_dt(&led0);
#endif
	}
}

static void uart_rx_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t c;

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &c, 1) != 1) {
			continue;
		}
		atomic_inc(&rx_byte_count);
		if (c == '\r' || c == '\n') {
			if (rx_idx > 0) {
				rx_buf[rx_idx] = '\0';
				process_line(rx_buf);
				rx_idx = 0;
			}
		} else if (rx_idx < RX_BUF_SZ - 1) {
			rx_buf[rx_idx++] = (char)c;
		} else {
			rx_idx = 0;
		}
	}
}

static void ping_timer_handler(struct k_timer *t)
{
	k_work_submit(&send_ping_work);
}
static K_TIMER_DEFINE(ping_timer, ping_timer_handler, NULL);

int main(void)
{
	printk("\n=== nrf-sw-lpuart symmetric ping test ===\n");

	if (!device_is_ready(dut_uart)) {
		LOG_ERR("dut-uart device %s not ready", dut_uart->name);
		return -1;
	}

#if HAS_LED0
	gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
#endif
#if HAS_LED1
	gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
#endif

	uart_irq_callback_user_data_set(dut_uart, uart_rx_cb, NULL);
	uart_irq_rx_enable(dut_uart);

	LOG_INF("Ready. Sending PING every 5 s; replying PING RECEIVED on incoming PING.");

	k_timer_start(&ping_timer, PING_INTERVAL, PING_INTERVAL);

	while (1) {
		k_sleep(K_SECONDS(1));
		LOG_INF("alive | rx total: %u bytes",
			(uint32_t)atomic_get(&rx_byte_count));
	}
	return 0;
}
