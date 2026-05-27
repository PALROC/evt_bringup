#
# Minimal isolation test for EVT2 9151 flash.
# Strips the board config down to ONLY SPI3 + the flash chip — no
# uart, no PMIC, no Wi-Fi pinctrl, no LPUART, nothing else. If THIS
# app reads the flash correctly, the EVT2 hardware is fine and our
# full bring-up firmware was causing the SPI3 wedge somehow. If it
# still fails, the wedge is genuinely about EVT2 hardware + the
# dual-master bus contention we already reproduced on stock DKs.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_test_evt2_minimal)

target_sources(app PRIVATE src/main.c)
