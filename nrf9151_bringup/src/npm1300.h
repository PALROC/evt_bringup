#ifndef NPM1300_H
#define NPM1300_H

/* Sample VBAT, NTC temperature, and average battery current from the
 * nPM1300's charger ADC and log them. Logs warnings on per-channel errors
 * but does not abort — partial reads are still useful.
 */
void npm1300_probe(void);

#endif
