/*
 * Server endpoint configuration for the reliability test.
 *
 * Edit these to point at YOUR VPS. The values below are placeholders.
 *
 * COAP_SERVER_HOST can be either:
 *   - a hostname (e.g. "heartbeat.palroc.com") — needs DNS to resolve
 *   - a literal IPv4 address (e.g. "203.0.113.42") — skips DNS
 *
 * Default CoAP UDP port is 5683.
 *
 * COAP_HEARTBEAT_PATH is the resource path the server listens on for
 * heartbeats; the server-side Python app maps requests on this path to
 * its CSV logger. /hb is short = fewer UDP bytes per heartbeat.
 *
 * COAP_HEARTBEAT_INTERVAL_S sets how often the device wakes up to send.
 * 300 s (5 min) is good for benchtop / fast failure detection; bump to
 * 900 s (15 min) for a low-power overnight run.
 */
#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#define COAP_SERVER_HOST           "158.179.212.244"
#define COAP_SERVER_PORT           5683
#define COAP_HEARTBEAT_PATH        "hb"

#define COAP_HEARTBEAT_INTERVAL_S  300   /* 5 minutes */

/* Per-transfer CoAP socket timeout (the modem can stall briefly on
 * a weak link). 30 s is generous; bumping to 60 s costs nothing
 * because we're sleep-gated by the interval above. */
#define COAP_REQUEST_TIMEOUT_MS    30000

#endif /* SERVER_CONFIG_H */
