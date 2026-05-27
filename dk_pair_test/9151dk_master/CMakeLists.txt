#
# nRF9151 DK side of the DK-pair shared-bus reproducer.
# Reads the onboard GD25WB256 flash via SPI3, prints JEDEC every 5 s.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dk_pair_9151_master)

target_sources(app PRIVATE src/main.c)
