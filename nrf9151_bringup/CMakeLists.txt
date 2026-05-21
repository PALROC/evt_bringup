#
# Copyright (c) 2016 Intel Corporation
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_connect_sdk_fundamentals)

target_sources(app PRIVATE
	src/main.c
	src/leds.c
	src/i2c_probe.c
	# src/npm1300.c — PMIC ownership moved to the L15; see prj.conf
	src/spi_probe.c
	src/modem.c
	src/gnss.c
	src/button_probe.c
	src/test_report.c
	src/diag.c
)
