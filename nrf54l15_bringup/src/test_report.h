/*
 * Copyright (c) 2025 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-board test reporting for the nRF54L15 sub-app. Mirrors the same
 * module in the 9151 sub-app at ../../src/test_report.{c,h}; copied
 * here because the two builds are independent Zephyr projects.
 */

#ifndef TEST_REPORT_H
#define TEST_REPORT_H

#include <stdarg.h>

typedef enum {
	TEST_PASS,
	TEST_FAIL,
	TEST_SKIP,
	TEST_INFO,
} test_result_t;

void test_report(const char *name, test_result_t result,
		 const char *detail_fmt, ...);
void test_report_summary(int board_number, const char *fw_version);

#endif
