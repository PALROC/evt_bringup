/*
 * Telemetry collection + CBOR encoding for the reliability heartbeat.
 *
 * One struct describes everything we want to send. telemetry_collect()
 * fills it in; telemetry_encode_cbor() serialises it to a CBOR map
 * the server can parse.
 *
 * Adding a field: add it to telemetry_t, fill it in telemetry_collect(),
 * encode it in telemetry_encode_cbor(). The server side is decoded by
 * CBOR-map key so order doesn't matter.
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	char     chip_id[17];    /* 8-byte hwinfo ID rendered as 16 hex + null */
	uint32_t uptime_s;       /* seconds since boot */
	uint32_t boot_count;     /* monotonic across resets (RAM noinit in v1) */

	/* --- battery / power (nPM1300) --- */
	int16_t  vbat_mV;        /* battery voltage in mV */
	int16_t  ibat_mA;        /* battery current in mA, signed:
	                          *   positive = charging (solar > load)
	                          *   negative = discharging */
	int8_t   ntc_C;          /* battery NTC temperature in °C */

	/* --- cellular --- */
	int16_t  rsrp_dBm;       /* serving-cell RSRP in dBm (negative) */
	char     rat[8];         /* "LTE-M" | "NB-IoT" | "" */
	int8_t   modem_C;        /* modem die temperature in °C */

	/* --- IMU (LSM6DSO over i2c2) --- */
	int16_t  accel_x_mg;     /* accel X, milli-g (gravity ≈ 1000) */
	int16_t  accel_y_mg;
	int16_t  accel_z_mg;
	int8_t   imu_C;          /* IMU die temperature in °C */
	uint8_t  imu_ok;         /* 1 = WHO_AM_I matched, 0 = not present / I²C error */

	/* --- system / debug --- */
	uint8_t  reset_reason;   /* hwinfo-mapped enum (compact byte) */
	char     fw_version[16]; /* short version string */
} telemetry_t;

/* Render hwinfo device-id into chip_id (16 hex chars + null). Idempotent. */
void telemetry_init(void);

/* Increment the boot counter and persist it. Call once at boot. */
void telemetry_bump_boot_count(void);

/* Collect all current telemetry into *t. */
void telemetry_collect(telemetry_t *t);

/* CBOR-encode *t into `buf` (capacity `buf_len`). Returns number of
 * bytes written, or negative on error. */
int  telemetry_encode_cbor(const telemetry_t *t, uint8_t *buf, size_t buf_len);

#endif /* TELEMETRY_H */
