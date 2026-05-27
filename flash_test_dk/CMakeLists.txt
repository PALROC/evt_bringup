#
# Standalone W25Q128JV JEDEC ID reader for the nRF9151 DK.
# Sibling app to nrf9151_bringup; same SPI logic, known-good hardware.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_test_dk)

target_sources(app PRIVATE src/main.c)
