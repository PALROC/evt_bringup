/*
 * Copyright (c) 2026 PALROC SL
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF Cloud device provisioning (A-GNSS Stage B). See provisioning.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <nrf_modem_at.h>
#include <date_time.h>
#if defined(CONFIG_NRF_PROVISIONING)
#include <net/nrf_provisioning.h>
#endif

#include "provisioning.h"
#include "test_report.h"

LOG_MODULE_REGISTER(provisioning, LOG_LEVEL_INF);

/* nRF Cloud device-credential sec_tag (default; matches the auto-
 * onboarding rule's KEYGEN/CMNG target). */
#define NRF_CLOUD_SEC_TAG 16842753

/* True once the device client cert is present at the nRF Cloud sec_tag. */
static bool client_cert_present(void)
{
	bool exists = false;
	int err = modem_key_mgmt_exists(NRF_CLOUD_SEC_TAG,
					MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
					&exists);
	if (err) {
		LOG_WRN("modem_key_mgmt_exists failed: %d", err);
		return false;
	}
	return exists;
}

#if defined(CONFIG_NRF_PROVISIONING)

/* The provisioning runs in two passes (KEYGEN+CSR submit, then the
 * service-signed client cert), so allow several trigger attempts with a
 * pause between them for the service to process the CSR. */
#define PROV_MAX_PASSES   4
#define PROV_PASS_GAP_S   15
#define PROV_DONE_TIMEOUT_S 90

static K_SEM_DEFINE(prov_done_sem, 0, 1);

/* The provisioning lib calls this to move the modem between online (to
 * talk to the service) and offline (to write credentials to NVM). Mirror
 * of the nrf_provisioning sample's handler. Returns the *previous* func
 * mode on success so the lib can restore it. */
static int modem_mode_cb(enum lte_lc_func_mode new_mode, void *user_data)
{
	enum lte_lc_func_mode cur;
	int ret;

	ARG_UNUSED(user_data);

	if (lte_lc_func_mode_get(&cur)) {
		LOG_ERR("lte_lc_func_mode_get failed");
		return -EFAULT;
	}
	if (cur == new_mode) {
		return cur;
	}
	if (new_mode == LTE_LC_FUNC_MODE_NORMAL) {
		ret = lte_lc_connect();
		if (ret) {
			LOG_ERR("provisioning: lte_lc_connect failed: %d", ret);
			return ret;
		}
		LOG_INF("provisioning: LTE reconnected");
		return cur;
	}
	ret = lte_lc_func_mode_set(new_mode);
	return ret ? ret : cur;
}

static void device_mode_cb(enum nrf_provisioning_event event, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event) {
	case NRF_PROVISIONING_EVENT_START:
		LOG_INF("provisioning: started");
		break;
	case NRF_PROVISIONING_EVENT_STOP:
		LOG_INF("provisioning: stopped (commands for this pass done)");
		break;
	case NRF_PROVISIONING_EVENT_DONE:
		LOG_INF("provisioning: DONE (finished received)");
		k_sem_give(&prov_done_sem);
		break;
	default:
		LOG_WRN("provisioning: unknown event %d", event);
		break;
	}
}

static struct nrf_provisioning_mm_change mmode = { .cb = modem_mode_cb };
static struct nrf_provisioning_dm_change dmode = { .cb = device_mode_cb };

bool provisioning_run(void)
{
	int err;

	if (client_cert_present()) {
		LOG_INF("device already provisioned (client cert at sec_tag %d)",
			NRF_CLOUD_SEC_TAG);
		test_report("provisioning", TEST_PASS, "already provisioned");
		return true;
	}

	LOG_INF("--- nRF Cloud provisioning (one-time OTA cert install) ---");

	/* Plain LTE-M (no GPS) so the modem can attach for the session. */
	nrf_modem_at_printf("AT+CFUN=0");
	(void)lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);

	/* The provisioning client needs an active LTE connection (to resolve
	 * + reach the service) AND valid network time (for the JWT/DTLS) BEFORE
	 * it's triggered — the lib's modem-mode callback only toggles
	 * offline/online to write creds, it does not do the initial attach. */
	LOG_INF("attaching LTE for provisioning...");
	err = lte_lc_connect();
	if (err) {
		LOG_ERR("LTE attach failed: %d", err);
		test_report("provisioning", TEST_FAIL, "LTE attach err %d", err);
		return false;
	}

	LOG_INF("waiting for network time...");
	(void)date_time_update_async(NULL);
	bool have_time = false;
	for (int i = 0; i < 30; i++) {
		int64_t ts;

		if (date_time_now(&ts) == 0) {
			have_time = true;
			break;
		}
		k_sleep(K_SECONDS(2));
	}
	if (!have_time) {
		LOG_WRN("no network time after 60s — provisioning may fail");
	} else {
		LOG_INF("network time acquired");
	}

	err = nrf_provisioning_init(&mmode, &dmode);
	if (err) {
		LOG_ERR("nrf_provisioning_init failed: %d", err);
		test_report("provisioning", TEST_FAIL, "init err %d", err);
		return false;
	}

	for (int pass = 1; pass <= PROV_MAX_PASSES; pass++) {
		LOG_INF("provisioning pass %d/%d...", pass, PROV_MAX_PASSES);
		k_sem_reset(&prov_done_sem);

		err = nrf_provisioning_trigger_manually();
		if (err) {
			LOG_ERR("trigger failed: %d", err);
			test_report("provisioning", TEST_FAIL, "trigger err %d",
				    err);
			return false;
		}

		if (k_sem_take(&prov_done_sem,
			       K_SECONDS(PROV_DONE_TIMEOUT_S)) != 0) {
			LOG_WRN("provisioning pass %d timed out", pass);
		}

		if (client_cert_present()) {
			LOG_INF("client cert installed — provisioning complete");
			test_report("provisioning", TEST_PASS,
				    "certs installed at sec_tag %d",
				    NRF_CLOUD_SEC_TAG);
			(void)lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);
			return true;
		}

		/* CSR was submitted this pass; give the service time to sign
		 * and queue the client cert before the next trigger. */
		LOG_INF("client cert not present yet; waiting %ds for the "
			"service to sign the CSR...", PROV_PASS_GAP_S);
		k_sleep(K_SECONDS(PROV_PASS_GAP_S));
	}

	LOG_ERR("provisioning did not complete after %d passes", PROV_MAX_PASSES);
	test_report("provisioning", TEST_FAIL, "no client cert after %d passes",
		    PROV_MAX_PASSES);
	return false;
}

#else /* !CONFIG_NRF_PROVISIONING */

/* Demo build: the heavy provisioning lib is excluded (RAM). Just report
 * whether the device already carries its nRF Cloud client cert. To
 * provision a new board, build once with overlay-provision.conf. */
bool provisioning_run(void)
{
	if (client_cert_present()) {
		LOG_INF("device provisioned (client cert at sec_tag %d)",
			NRF_CLOUD_SEC_TAG);
		test_report("provisioning", TEST_PASS, "already provisioned");
		return true;
	}

	LOG_WRN("device NOT provisioned — build with overlay-provision.conf "
		"and run once to install nRF Cloud credentials");
	test_report("provisioning", TEST_FAIL, "not provisioned (use overlay)");
	return false;
}

#endif /* CONFIG_NRF_PROVISIONING */
