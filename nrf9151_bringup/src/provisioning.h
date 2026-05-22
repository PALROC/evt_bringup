#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

/* nRF Cloud device provisioning (A-GNSS Stage B).
 *
 * One-time per board: pulls the KEYGEN + certificate commands queued by
 * the device's auto-onboarding rule on nRF Cloud, over a DTLS CoAP link
 * to the provisioning service, and writes the resulting device cert/key
 * + CA into the modem keystore at the nRF Cloud sec_tag (16842753).
 *
 * Idempotent: if the device client cert already exists at that sec_tag,
 * this returns immediately (already provisioned). Otherwise it runs the
 * provisioning client through its two passes (KEYGEN/CSR, then the
 * signed client cert) until the cert appears or it times out.
 *
 * Requires the modem to be initialised (call after modem_probe()). The
 * provisioning client manages the LTE link itself during the session
 * (it must take the modem offline to write credentials). Leaves the
 * modem at CFUN=0 on return.
 *
 * Returns true if the device is provisioned (client cert present) on
 * exit, false otherwise. Files a "provisioning" test_report.
 */
bool provisioning_run(void);

#endif /* PROVISIONING_H */
