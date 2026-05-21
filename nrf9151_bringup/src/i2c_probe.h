#ifndef I2C_PROBE_H
#define I2C_PROBE_H

#include <stdbool.h>

/* Returns true if i2c2 is ready to use. */
bool i2c_probe_ready(void);

/* Scan i2c2 from 0x03..0x77 using zero-byte writes; logs each ACK. */
void i2c_scan(void);

#endif
