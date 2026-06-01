#
# nRF9151 reliability soak test — periodic CoAP heartbeats to a VPS.
#
# A small standalone app (not the full bring-up demo) that:
#   1. Attaches LTE-M once at boot
#   2. Every N minutes:
#      - samples telemetry (uptime, VBAT, RSRP, free heap, reset reason)
#      - CBOR-encodes a heartbeat payload
#      - POSTs it to a CoAP server via UDP
#      - returns the modem to PSM sleep between heartbeats
#   3. Re-establishes the connection if it drops
#
# Each device identifies itself via hwinfo_get_device_id() (8 bytes,
# durable per SoC) so the server can dashboard one row per board.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reliability_test)

target_sources(app PRIVATE
	src/main.c
	src/telemetry.c
	src/coap_client.c
)
