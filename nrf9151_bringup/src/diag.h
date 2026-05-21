#ifndef DIAG_H
#define DIAG_H

/* Log the reset reason from the SoC's RESETREAS register (via hwinfo).
 * Call once at the very start of main, before doing anything else, so
 * the cause of the previous reset is preserved.
 */
void diag_print_reset_reason(void);

#endif
