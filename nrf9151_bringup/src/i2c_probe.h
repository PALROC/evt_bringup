#ifndef I2C_PROBE_H
#define I2C_PROBE_H

#include <stdbool.h>

/* Returns true if i2c2 is ready to use. */
bool i2c_probe_ready(void);

/* Scan i2c2 from 0x03..0x77 using zero-byte writes; logs each ACK. */
void i2c_scan(void);

/* Read LSM6DSO WHO_AM_I over i2c2 (addr 0x6a, EVT2 wiring), log + PASS/FAIL.
 * EVT2 moved the IMU off SPI3 onto i2c2; the SPI variant is
 * spi_imu_whoami() and is selected on EVT1 (see main.c). */
void i2c_imu_whoami(void);

#endif
