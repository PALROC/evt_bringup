#
# nRF9151 DK SILENT firmware — boots, prints, and does nothing.
# Companion to ../9151dk_master/. Use this when you want the 9151 DK
# powered and running but NOT touching the shared SPI bus, so the L15
# DK can drive the bus alone.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dk_pair_9151_silent)

target_sources(app PRIVATE src/main.c)
