#ifndef BUTTON_PROBE_H
#define BUTTON_PROBE_H

/* Visualise the hall-sensor button on P0.01 for `duration_seconds`.
 * Configures the pin as input with an internal pull-up (safe whether the
 * sensor is open-drain or push-pull) and logs every level transition,
 * plus a 1-Hz heartbeat showing the current level. Mirrors the level to
 * the red LED for live feedback while you wave a magnet over it.
 *
 * Output tells you:
 *   - the resting level (idle reading: HIGH or LOW)
 *   - the active level (changes when magnet is near)
 *   - whether each magnet pass produces 1 transition or several
 *     (bouncy / multi-pole magnet behaviour shows up here)
 *   - the total transitions over the test window
 */
void button_probe(int duration_seconds);

#endif
