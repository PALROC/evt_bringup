/*
 * Copyright (c) 2025 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "test_report.h"

LOG_MODULE_REGISTER(test_report, LOG_LEVEL_INF);

#define MAX_TESTS       24
#define MAX_NAME_LEN    24
#define MAX_DETAIL_LEN  72

struct test_record {
	char           name[MAX_NAME_LEN];
	test_result_t  result;
	char           detail[MAX_DETAIL_LEN];
};

static struct test_record records[MAX_TESTS];
static int                n_records;
static bool               overflow_warned;

void test_report(const char *name, test_result_t result,
		 const char *detail_fmt, ...)
{
	if (n_records >= MAX_TESTS) {
		if (!overflow_warned) {
			LOG_WRN("test_report: capacity reached (%d), dropping further reports",
				MAX_TESTS);
			overflow_warned = true;
		}
		return;
	}

	struct test_record *r = &records[n_records++];

	strncpy(r->name, name ? name : "(unnamed)", sizeof(r->name) - 1);
	r->name[sizeof(r->name) - 1] = '\0';
	r->result = result;

	if (detail_fmt) {
		va_list ap;

		va_start(ap, detail_fmt);
		vsnprintf(r->detail, sizeof(r->detail), detail_fmt, ap);
		va_end(ap);
	} else {
		r->detail[0] = '\0';
	}
}

static const char *result_str(test_result_t r)
{
	switch (r) {
	case TEST_PASS: return "PASS";
	case TEST_FAIL: return "FAIL";
	case TEST_SKIP: return "SKIP";
	case TEST_INFO: return "INFO";
	default:        return "????";
	}
}

void test_report_summary(int board_number, const char *fw_version)
{
	int pass = 0, fail = 0, skip = 0, info = 0;

	LOG_INF("");
	LOG_INF("=========================================================");
	LOG_INF("=== BOARD %d  fw=%s  TEST SUMMARY ===",
		board_number, fw_version ? fw_version : "?");
	LOG_INF("=========================================================");
	for (int i = 0; i < n_records; i++) {
		switch (records[i].result) {
		case TEST_PASS: pass++; break;
		case TEST_FAIL: fail++; break;
		case TEST_SKIP: skip++; break;
		case TEST_INFO: info++; break;
		}
		LOG_INF("  [%s] %-22s %s",
			result_str(records[i].result),
			records[i].name,
			records[i].detail);
	}
	LOG_INF("---------------------------------------------------------");
	LOG_INF("=== BOARD %d totals: PASS=%d  FAIL=%d  INFO=%d  SKIP=%d ===",
		board_number, pass, fail, info, skip);
	LOG_INF("=========================================================");
	LOG_INF("");
}
