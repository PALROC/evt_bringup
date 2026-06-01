/*
 * Minimal CoAP client for the reliability heartbeat.
 *
 * One persistent UDP socket to the server; one POST per heartbeat
 * carrying CBOR. No DTLS in v1 (server is on a public VPS UDP port —
 * acceptable for an unencrypted heartbeat test, will upgrade later).
 */
#ifndef COAP_CLIENT_H
#define COAP_CLIENT_H

#include <stdint.h>
#include <stddef.h>

/* Resolve the server hostname, open the UDP socket. Idempotent: safe
 * to call again after the previous socket dropped. Returns 0 on
 * success, negative errno otherwise. */
int coap_client_connect(void);

/* Close the socket. Call before suspending the modem if we want to
 * fully tear down between heartbeats. (Optional; the modem PSM path
 * can keep the socket alive too — we try keep-alive first and fall
 * back to connect-each-time if that's flaky.) */
void coap_client_disconnect(void);

/* POST `payload`/`len` bytes (already CBOR-encoded) to the
 * heartbeat path on the server. Returns 0 on success (server returned
 * 2.xx), negative errno otherwise. */
int coap_client_post_heartbeat(const uint8_t *payload, size_t len);

#endif /* COAP_CLIENT_H */
