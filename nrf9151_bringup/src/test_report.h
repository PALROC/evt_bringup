/*
 * Copyright (c) 2025 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_REPORT_H
#define TEST_REPORT_H

#include <stdarg.h>

typedef enum {
	TEST_PASS,  /* feature works as intended on this board */
	TEST_FAIL,  /* feature does not work — needs investigation */
	TEST_SKIP,  /* feature was not attempted (e.g. probe disabled) */
	TEST_INFO,  /* informational only (no pass/fail criterion, e.g. GNSS CN0) */
} test_result_t;

/* Record one test result. `name` and `detail_fmt` (+ args) are copied into
 * an internal table — the caller doesn't need to keep the strings alive.
 * Calls beyond the table capacity are silently dropped (warns once).
 */
void test_report(const char *name, test_result_t result,
		 const char *detail_fmt, ...);

/* Print a summary table of every test reported so far, prefixed with the
 * board number + firmware version. Includes a one-line pass/fail count.
 * Designed to be visually distinctive in the RTT log so you can scroll
 * straight to it after every board test.
 */
void test_report_summary(int board_number, const char *fw_version);

#endif
