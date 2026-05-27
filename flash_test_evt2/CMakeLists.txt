#
# SPI flash JEDEC ID probe from the nRF9151 side on EVT2.
# Sibling of flash_test_evt1 (same SPI3 pinout, different board target).
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_test_evt2)

target_sources(app PRIVATE src/main.c)
