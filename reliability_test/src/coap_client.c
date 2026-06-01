/*
 * Plain (non-DTLS) CoAP POST client for the heartbeat.
 *
 * Uses the Zephyr CoAP library at the low-level packet API. (We're
 * not using coap_client_req in v1 because we want explicit control
 * over the UDP socket for fast PSM-aware reconnect logic.)
 *
 * Flow per heartbeat:
 *   1. If socket is closed, resolve DNS + connect
 *   2. Build a CON POST with path = "hb" and CBOR payload
 *   3. Send via sendto()
 *   4. recvfrom() with timeout; parse response code
 *   5. Return 0 on a 2.xx response, negative otherwise
 *
 * The modem PSM behaviour means socket DURING idle is typically OK —
 * the modem keeps its EPS bearer attached and we just sit. If the
 * tower drops us, the next sendto() returns an error and the caller
 * is expected to reconnect.
 */

#include "coap_client.h"
#include "server_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(coap_client, LOG_LEVEL_INF);

#define COAP_VERSION       1
#define COAP_TOKEN_LEN     4
#define MAX_COAP_MSG_LEN   512

static int sock = -1;
static struct sockaddr_storage server_addr;

static int resolve_and_setup_address(void)
{
	struct addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	struct addrinfo *res;

	int err = getaddrinfo(COAP_SERVER_HOST, NULL, &hints, &res);
	if (err) {
		LOG_ERR("getaddrinfo(%s) failed: %d", COAP_SERVER_HOST, err);
		return -EHOSTUNREACH;
	}

	struct sockaddr_in *server = (struct sockaddr_in *)&server_addr;
	server->sin_family = AF_INET;
	server->sin_port   = htons(COAP_SERVER_PORT);
	memcpy(&server->sin_addr,
	       &((struct sockaddr_in *)res->ai_addr)->sin_addr,
	       sizeof(server->sin_addr));

	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &server->sin_addr, ip_str, sizeof(ip_str));
	LOG_INF("resolved %s -> %s:%d",
		COAP_SERVER_HOST, ip_str, COAP_SERVER_PORT);

	freeaddrinfo(res);
	return 0;
}

int coap_client_connect(void)
{
	if (sock >= 0) {
		return 0;     /* already connected */
	}

	int err = resolve_and_setup_address();
	if (err) {
		return err;
	}

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return -errno;
	}

	struct timeval tv = {
		.tv_sec  = COAP_REQUEST_TIMEOUT_MS / 1000,
		.tv_usec = (COAP_REQUEST_TIMEOUT_MS % 1000) * 1000,
	};
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		LOG_WRN("SO_RCVTIMEO setsockopt failed: %d", errno);
	}

	LOG_INF("coap_client UDP socket ready (fd=%d)", sock);
	return 0;
}

void coap_client_disconnect(void)
{
	if (sock >= 0) {
		close(sock);
		sock = -1;
		LOG_INF("coap_client socket closed");
	}
}

int coap_client_post_heartbeat(const uint8_t *payload, size_t len)
{
	if (sock < 0) {
		int err = coap_client_connect();
		if (err) {
			return err;
		}
	}

	/* --- Build CoAP CON POST -------------------------------------- */
	uint8_t coap_buf[MAX_COAP_MSG_LEN];
	struct coap_packet pkt;

	/* Random-ish token per request; helps server correlate. We don't
	 * need cryptographic randomness — uptime+counter is plenty for
	 * "uniquely identify which response goes with which request".
	 * Avoids pulling in CONFIG_ENTROPY_GENERATOR for sys_rand32_get. */
	static uint32_t token_seed;
	uint16_t msg_id = coap_next_id();
	uint8_t  token[COAP_TOKEN_LEN];
	uint32_t token_val = (uint32_t)k_uptime_get_32() ^ (++token_seed);
	memcpy(token, &token_val, sizeof(token));

	int r = coap_packet_init(&pkt, coap_buf, sizeof(coap_buf),
				 COAP_VERSION, COAP_TYPE_CON,
				 COAP_TOKEN_LEN, token,
				 COAP_METHOD_POST, msg_id);
	if (r < 0) {
		LOG_ERR("coap_packet_init failed: %d", r);
		return r;
	}

	r = coap_packet_append_option(&pkt, COAP_OPTION_URI_PATH,
				      COAP_HEARTBEAT_PATH,
				      strlen(COAP_HEARTBEAT_PATH));
	if (r < 0) {
		LOG_ERR("append URI_PATH failed: %d", r);
		return r;
	}

	uint8_t content_format = COAP_CONTENT_FORMAT_APP_CBOR;
	r = coap_packet_append_option(&pkt, COAP_OPTION_CONTENT_FORMAT,
				      &content_format, sizeof(content_format));
	if (r < 0) {
		LOG_ERR("append CONTENT_FORMAT failed: %d", r);
		return r;
	}

	r = coap_packet_append_payload_marker(&pkt);
	if (r < 0) {
		LOG_ERR("append payload marker failed: %d", r);
		return r;
	}

	r = coap_packet_append_payload(&pkt, payload, len);
	if (r < 0) {
		LOG_ERR("append payload failed: %d", r);
		return r;
	}

	/* --- Send ----------------------------------------------------- */
	ssize_t sent = sendto(sock, pkt.data, pkt.offset, 0,
			      (struct sockaddr *)&server_addr,
			      sizeof(struct sockaddr_in));
	if (sent < 0) {
		LOG_ERR("sendto failed: %d", errno);
		coap_client_disconnect();        /* force reconnect next time */
		return -errno;
	}
	LOG_INF("CoAP POST sent (%d bytes; payload %u)",
		(int)sent, (unsigned)len);

	/* --- Wait for ACK + response code ---------------------------- */
	uint8_t resp_buf[256];
	ssize_t got = recv(sock, resp_buf, sizeof(resp_buf), 0);
	if (got < 0) {
		LOG_ERR("recv failed: %d (timeout? server down?)", errno);
		coap_client_disconnect();
		return -errno;
	}

	struct coap_packet resp;
	r = coap_packet_parse(&resp, resp_buf, got, NULL, 0);
	if (r < 0) {
		LOG_ERR("coap_packet_parse failed: %d", r);
		return r;
	}

	/* CoAP response codes pack the class in the top 3 bits, detail in
	 * the bottom 5. The macro COAP_RESPONSE_CODE_CLASS doesn't exist
	 * in this NCS, so we just shift. Class 2 = success (2.01, 2.04, ...). */
	uint8_t code = coap_header_get_code(&resp);
	uint8_t cls  = (code >> 5) & 0x07;
	uint8_t det  = code & 0x1f;
	if (cls == 2) {
		LOG_INF("CoAP response code %u.%02u (OK)", cls, det);
		return 0;
	}
	LOG_WRN("CoAP response code %u.%02u (not 2.xx)", cls, det);
	return -EBADMSG;
}
