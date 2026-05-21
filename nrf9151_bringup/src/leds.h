#ifndef LEDS_H
#define LEDS_H

/* Pulse the red LED for ~1 s. Use as a boot/reset visual marker. */
void boot_indicator(void);

/* Walk blue -> green -> red, 1 s on / 1 s off, 3 cycles each. */
void leds_walk(void);

#endif
