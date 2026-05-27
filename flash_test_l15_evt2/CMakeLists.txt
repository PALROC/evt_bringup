#
# SPI flash JEDEC ID probe from the nRF54L15 side on EVT2.
# Mirror of flash_test_evt1, but driven by the L15's spi00 instead
# of the 9151's SPI3 — testing the shared flash bus from the other master.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_test_l15_evt2)

target_sources(app PRIVATE src/main.c)
