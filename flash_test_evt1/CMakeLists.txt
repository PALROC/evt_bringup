#
# SPI flash JEDEC ID probe for the EVT1 board.
# Sibling app to flash_test_dk; same JEDEC code, EVT1 pin map.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_test_evt1)

target_sources(app PRIVATE src/main.c)
