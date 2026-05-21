#ifndef SPI_PROBE_H
#define SPI_PROBE_H

#include <stdbool.h>

/* Returns true if spi3 is ready. */
bool spi_probe_ready(void);

/* Read JEDEC ID from the W25Q128JV on CS=P0.12, log result + PASS/FAIL. */
void spi_flash_jedec(void);

/* Read LSM6DSO WHO_AM_I from CS=P0.16, log result + PASS/FAIL. */
void spi_imu_whoami(void);

#endif
