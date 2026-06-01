/*
 * Telemetry collection + CBOR encoding.
 *
 * See telemetry.h for what the heartbeat carries. The CBOR layout is a
 * single map keyed by short strings so the server-side decoder can use
 * any standard CBOR library (cbor2 in Python).
 *
 * Map keys (kept short for tiny LTE-M payloads):
 *   "id"   tstr     16-char hex chip ID
 *   "up"   uint     uptime seconds
 *   "boot" uint     boot count
 *   "vbat" int      VBAT in millivolts
 *   "ibat" int      IBAT in milliamps (signed; + = charging)
 *   "ntc"  int      battery NTC temperature in °C
 *   "rsrp" int      RSRP in dBm (negative)
 *   "rat"  tstr     "LTE-M" / "NB-IoT" / ""
 *   "mC"   int      modem die temperature in °C
 *   "ax"   int      accel X, milli-g (gravity ≈ 1000)
 *   "ay"   int      accel Y, milli-g
 *   "az"   int      accel Z, milli-g
 *   "iC"   int      IMU die temperature in °C
 *   "iok"  uint     1 = IMU read succeeded, 0 = failed
 *   "rst"  uint     reset reason (compact enum byte)
 *   "fw"   tstr     firmware version short string
 *
 * 16 keys × ~10-12 bytes average → ~160-190 bytes CBOR. Plus CoAP
 * header + UDP/IP overhead ~ 230-260 bytes per heartbeat. Still
 * well under the LTE-M IP MTU of 1280.
 */

#include "telemetry.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <zcbor_encode.h>

#include <modem/modem_info.h>
#include <modem/lte_lc.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(telemetry, LOG_LEVEL_INF);

/* Persistent boot counter — kept in a Zephyr `__noinit` RAM area for
 * simplicity in v1 (survives soft resets but NOT power loss). For
 * persistent-across-power-cycles we'd use NVS or settings; deferred.
 * The CSV server log records its own server-side count anyway. */
static __noinit uint32_t persistent_boot_count;
static __noinit uint32_t persistent_boot_count_check;

#define BOOT_COUNT_MAGIC  0xCAFEFACEU

#define FW_VERSION_STR    "rel-test-0.1.0"

static char  s_chip_id[17];
static uint8_t s_reset_reason;

static void compute_chip_id(void)
{
	uint8_t id[8] = { 0 };
	ssize_t got = hwinfo_get_device_id(id, sizeof(id));
	if (got <= 0) {
		strncpy(s_chip_id, "unknown000000000", sizeof(s_chip_id));
		return;
	}
	/* Pad / clip to 8 bytes. */
	size_t n = (got > 8) ? 8 : (size_t)got;
	for (size_t i = 0; i < n; i++) {
		snprintf(&s_chip_id[i * 2], 3, "%02x", id[i]);
	}
	s_chip_id[16] = '\0';
}

static uint8_t fetch_reset_reason(void)
{
	uint32_t cause = 0;
	(void)hwinfo_get_reset_cause(&cause);
	(void)hwinfo_clear_reset_cause();
	/* The hwinfo cause is a bitmask, but we only need a compact single-
	 * byte enum for telemetry. Map by priority: power-on > brown-out >
	 * pin > soft > watchdog > everything else. */
	if (cause & RESET_POR)        return 1;
	if (cause & RESET_BROWNOUT)   return 2;
	if (cause & RESET_PIN)        return 3;
	if (cause & RESET_SOFTWARE)   return 4;
	if (cause & RESET_WATCHDOG)   return 5;
	if (cause & RESET_LOW_POWER_WAKE) return 6;
	if (cause)                    return 7;   /* "other" */
	return 0;                                   /* unknown */
}

void telemetry_init(void)
{
	compute_chip_id();
	s_reset_reason = fetch_reset_reason();

	LOG_INF("telemetry: chip_id=%s reset_reason=%u fw=%s",
		s_chip_id, s_reset_reason, FW_VERSION_STR);
}

void telemetry_bump_boot_count(void)
{
	if (persistent_boot_count_check != BOOT_COUNT_MAGIC) {
		persistent_boot_count = 0;
		persistent_boot_count_check = BOOT_COUNT_MAGIC;
	}
	persistent_boot_count++;
	LOG_INF("telemetry: boot_count=%u (this session)", persistent_boot_count);
}

/* nPM1300 charger device — see board's common.dtsi for the node. */
static const struct device *const charger =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(npm1300_charger));

/* Read VBAT, IBAT, NTC from the nPM1300 charger sensor. On error
 * leaves the destination fields at 0 and logs once. */
static void read_npm1300(telemetry_t *t)
{
	if (!charger) {
		return;   /* DT didn't declare the node */
	}
	if (!device_is_ready(charger)) {
		LOG_WRN("nPM1300 charger not ready (i2c not up yet?)");
		return;
	}

	int err = sensor_sample_fetch(charger);
	if (err) {
		LOG_WRN("nPM1300 sample_fetch failed: %d", err);
		return;
	}

	struct sensor_value v;

	if (sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &v) == 0) {
		/* val1=volts, val2=micro-volts. Convert to mV. */
		t->vbat_mV = (int16_t)(v.val1 * 1000 + v.val2 / 1000);
	}

	if (sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &v) == 0) {
		/* val1=amps, val2=micro-amps. Convert to mA. Sign is
		 * preserved by the driver (negative = discharge). */
		t->ibat_mA = (int16_t)(v.val1 * 1000 + v.val2 / 1000);
	}

	if (sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &v) == 0) {
		/* val1=°C (integer part), val2=micro-degrees. We only
		 * need integer °C. */
		t->ntc_C = (int8_t)v.val1;
	}
}

/* --- LSM6DSO IMU on i2c2 (EVT2 only — EVT1 had it on SPI3) ----------
 *
 * We hit the device with raw I²C register reads, same pattern as the
 * bring-up i2c_probe.c. No Zephyr sensor-driver dependency — pulling
 * in CONFIG_SENSOR_LSM6DSO would add a non-trivial amount of code for
 * what's effectively four register reads. Cheaper to talk raw.
 *
 * Quick map of the registers we touch:
 *   0x0F WHO_AM_I    -> 0x6C
 *   0x10 CTRL1_XL    -> 0x40 = 104 Hz, ±2 g (set once if not running)
 *   0x20 CTRL3_C     -> enable BDU (block-data-update) to prevent
 *                       tearing between the two bytes of each axis
 *   0x20 OUT_TEMP_L  -> 16-bit signed temp, LSB = 1/256 °C, offset 25°C
 *   0x28 OUTX_L_A    -> 6 bytes of XYZ accel, LSB = 0.061 mg
 *
 * No reset / sleep handling — the chip is already powered, and once
 * CTRL1_XL is set it stays in continuous-conversion mode across our
 * 5-minute heartbeat interval. */
#define LSM6DSO_I2C_ADDR        0x6a
#define LSM6DSO_REG_WHO_AM_I    0x0F
#define LSM6DSO_WHO_AM_I_VAL    0x6C
#define LSM6DSO_REG_CTRL1_XL    0x10
#define LSM6DSO_CTRL1_XL_104_2G 0x40
#define LSM6DSO_REG_CTRL3_C     0x12
#define LSM6DSO_CTRL3_C_BDU     0x40   /* BDU=1, IF_INC=0 (we set IF_INC separately on burst reads) */
#define LSM6DSO_REG_OUT_TEMP_L  0x20
#define LSM6DSO_REG_OUTX_L_A    0x28

/* 0.061 mg per LSB at ±2 g full scale (LSM6DSO datasheet §4.1). */
#define LSM6DSO_ACCEL_LSB_MG_X1000  61   /* milli-g × 1000, so mg = (raw * 61) / 1000 */

static bool imu_configured;

static const struct device *const i2c2 =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c2));

static int imu_ensure_configured(void)
{
	if (imu_configured) {
		return 0;
	}
	if (!i2c2 || !device_is_ready(i2c2)) {
		return -ENODEV;
	}

	uint8_t who = 0;
	int ret = i2c_reg_read_byte(i2c2, LSM6DSO_I2C_ADDR,
				    LSM6DSO_REG_WHO_AM_I, &who);
	if (ret || who != LSM6DSO_WHO_AM_I_VAL) {
		LOG_WRN("LSM6DSO WHO_AM_I read failed: ret=%d val=0x%02x",
			ret, who);
		return ret ? ret : -ENODEV;
	}

	/* 104 Hz / ±2 g — same setup the bring-up used. Block-data-update
	 * so a fast caller can't read a half-updated 16-bit sample. */
	(void)i2c_reg_write_byte(i2c2, LSM6DSO_I2C_ADDR,
				 LSM6DSO_REG_CTRL1_XL,
				 LSM6DSO_CTRL1_XL_104_2G);
	(void)i2c_reg_write_byte(i2c2, LSM6DSO_I2C_ADDR,
				 LSM6DSO_REG_CTRL3_C,
				 LSM6DSO_CTRL3_C_BDU | 0x04 /*IF_INC=1*/);

	imu_configured = true;
	LOG_INF("LSM6DSO configured (104 Hz, ±2 g, BDU)");
	return 0;
}

static void read_imu(telemetry_t *t)
{
	if (imu_ensure_configured() != 0) {
		t->imu_ok = 0;
		return;
	}

	/* Accel XYZ: 6 bytes little-endian signed. With CTRL3_C.IF_INC=1
	 * we can burst-read across the auto-incrementing register window. */
	uint8_t buf[6];
	int ret = i2c_burst_read(i2c2, LSM6DSO_I2C_ADDR,
				 LSM6DSO_REG_OUTX_L_A, buf, sizeof(buf));
	if (ret) {
		LOG_WRN("IMU accel burst-read failed: %d", ret);
		t->imu_ok = 0;
		return;
	}
	int16_t raw_x = (int16_t)(buf[0] | (buf[1] << 8));
	int16_t raw_y = (int16_t)(buf[2] | (buf[3] << 8));
	int16_t raw_z = (int16_t)(buf[4] | (buf[5] << 8));

	/* Convert raw LSB to milli-g. 0.061 mg/LSB → multiply by 61, /1000.
	 * Worst-case raw value ±32768 × 61 = ±2.0e6, fits in int32_t. */
	t->accel_x_mg = (int16_t)(((int32_t)raw_x * LSM6DSO_ACCEL_LSB_MG_X1000) / 1000);
	t->accel_y_mg = (int16_t)(((int32_t)raw_y * LSM6DSO_ACCEL_LSB_MG_X1000) / 1000);
	t->accel_z_mg = (int16_t)(((int32_t)raw_z * LSM6DSO_ACCEL_LSB_MG_X1000) / 1000);

	/* IMU die temp: 16-bit signed, LSB = 1/256 °C, zero-offset 25°C
	 * (LSM6DSO datasheet §4.3). So °C = 25 + raw/256. */
	uint8_t tbuf[2];
	ret = i2c_burst_read(i2c2, LSM6DSO_I2C_ADDR,
			     LSM6DSO_REG_OUT_TEMP_L, tbuf, sizeof(tbuf));
	if (ret == 0) {
		int16_t raw_t = (int16_t)(tbuf[0] | (tbuf[1] << 8));
		t->imu_C = (int8_t)(25 + (raw_t / 256));
	}

	t->imu_ok = 1;
}

/* Read cellular RAT + RSRP + modem die temperature. Robust to the
 * modem not being attached yet (just leaves fields zeroed). */
static void read_cellular(telemetry_t *t)
{
	/* --- RAT via lte_lc API (more reliable than MODEM_INFO_LTE_MODE
	 * which can return stale data right after attach) -------------- */
	enum lte_lc_lte_mode mode = LTE_LC_LTE_MODE_NONE;
	int err = lte_lc_lte_mode_get(&mode);
	if (err == 0) {
		switch (mode) {
		case LTE_LC_LTE_MODE_LTEM:
			strncpy(t->rat, "LTE-M", sizeof(t->rat) - 1);
			break;
		case LTE_LC_LTE_MODE_NBIOT:
			strncpy(t->rat, "NB-IoT", sizeof(t->rat) - 1);
			break;
		default:
			t->rat[0] = '\0';
			break;
		}
	}

	/* --- RSRP: modem_info returns the RAW INDEX (0..97), not dBm.
	 * Conversion per 3GPP TS 36.133 §9.1.4:
	 *     RSRP[dBm] = index - 140
	 * So index 39 → -101 dBm, index 97 → -43 dBm. Index 255 means
	 * "not measured". ----------------------------------------------- */
	char buf[32] = { 0 };
	int len = modem_info_string_get(MODEM_INFO_RSRP, buf, sizeof(buf));
	if (len > 0) {
		int idx = atoi(buf);
		if (idx > 0 && idx < 255) {
			t->rsrp_dBm = (int16_t)(idx - 140);
		}
	}

	/* --- Modem die temperature via AT+CTEMP / modem_info ---------- */
	len = modem_info_string_get(MODEM_INFO_TEMP, buf, sizeof(buf));
	if (len > 0) {
		t->modem_C = (int8_t)atoi(buf);
	}
}

void telemetry_collect(telemetry_t *t)
{
	memset(t, 0, sizeof(*t));

	strncpy(t->chip_id, s_chip_id, sizeof(t->chip_id));
	t->uptime_s    = k_uptime_get() / 1000;
	t->boot_count  = persistent_boot_count;
	t->reset_reason = s_reset_reason;
	strncpy(t->fw_version, FW_VERSION_STR, sizeof(t->fw_version) - 1);

	read_npm1300(t);
	read_cellular(t);
	read_imu(t);
}

int telemetry_encode_cbor(const telemetry_t *t, uint8_t *buf, size_t buf_len)
{
	ZCBOR_STATE_E(state, 1 /*map*/, buf, buf_len, 16);

	bool ok = true;
	ok = ok && zcbor_map_start_encode(state, 16);

	ok = ok && zcbor_tstr_put_lit(state, "id");
	ok = ok && zcbor_tstr_put_term(state, t->chip_id, sizeof(t->chip_id));

	ok = ok && zcbor_tstr_put_lit(state, "up");
	ok = ok && zcbor_uint32_put(state, t->uptime_s);

	ok = ok && zcbor_tstr_put_lit(state, "boot");
	ok = ok && zcbor_uint32_put(state, t->boot_count);

	ok = ok && zcbor_tstr_put_lit(state, "vbat");
	ok = ok && zcbor_int32_put(state, t->vbat_mV);

	ok = ok && zcbor_tstr_put_lit(state, "ibat");
	ok = ok && zcbor_int32_put(state, t->ibat_mA);

	ok = ok && zcbor_tstr_put_lit(state, "ntc");
	ok = ok && zcbor_int32_put(state, t->ntc_C);

	ok = ok && zcbor_tstr_put_lit(state, "rsrp");
	ok = ok && zcbor_int32_put(state, t->rsrp_dBm);

	ok = ok && zcbor_tstr_put_lit(state, "rat");
	ok = ok && zcbor_tstr_put_term(state, t->rat, sizeof(t->rat));

	ok = ok && zcbor_tstr_put_lit(state, "mC");
	ok = ok && zcbor_int32_put(state, t->modem_C);

	ok = ok && zcbor_tstr_put_lit(state, "ax");
	ok = ok && zcbor_int32_put(state, t->accel_x_mg);

	ok = ok && zcbor_tstr_put_lit(state, "ay");
	ok = ok && zcbor_int32_put(state, t->accel_y_mg);

	ok = ok && zcbor_tstr_put_lit(state, "az");
	ok = ok && zcbor_int32_put(state, t->accel_z_mg);

	ok = ok && zcbor_tstr_put_lit(state, "iC");
	ok = ok && zcbor_int32_put(state, t->imu_C);

	ok = ok && zcbor_tstr_put_lit(state, "iok");
	ok = ok && zcbor_uint32_put(state, t->imu_ok);

	ok = ok && zcbor_tstr_put_lit(state, "rst");
	ok = ok && zcbor_uint32_put(state, t->reset_reason);

	ok = ok && zcbor_tstr_put_lit(state, "fw");
	ok = ok && zcbor_tstr_put_term(state, t->fw_version, sizeof(t->fw_version));

	ok = ok && zcbor_map_end_encode(state, 16);

	if (!ok) {
		LOG_ERR("CBOR encode failed");
		return -EINVAL;
	}
	return (int)(state->payload - buf);
}
