#
# EVT2 SPIM3 raw-register test. Bypasses Zephyr SPI driver AND nrfx_spim
# driver — talks directly to the SPIM3 hardware via Nordic's nrf_spim HAL
# (just thin macros over the register addresses).
#
# Companion to flash_test_evt2_minimal. If the Zephyr SPI driver fails
# but THIS app's raw register sequence works, the bug is in the Zephyr
# driver. If THIS also fails, the bug is silicon / TF-M / clock-level.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_test_evt2_raw_spim)

target_sources(app PRIVATE src/main.c)
