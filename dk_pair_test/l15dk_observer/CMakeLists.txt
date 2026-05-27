#
# nRF54L15 DK side of the DK-pair shared-bus reproducer.
# Pairs with ../9151dk_master/ on a wired 9151 DK.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dk_pair_l15_observer)

target_sources(app PRIVATE src/main.c)
