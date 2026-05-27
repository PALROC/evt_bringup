#
# nRF9151 DK BIT-BANG reader for the onboard GD25WB256 flash.
# Bypasses SPI3 entirely and toggles the four pins as plain GPIOs.
# Useful as a ground-truth sanity check that the 9151 DK's onboard
# wiring to the GD25 is intact.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dk_pair_9151_bitbang)

target_sources(app PRIVATE src/main.c)
