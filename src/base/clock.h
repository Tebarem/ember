#pragma once
/*
 * The monotonic clock (W5c). Timers, performance.now() and Event.timeStamp read it.
 *
 * A fake clock replaces the real one for tests: embi_clock_set_fake(true, start) freezes time at start, and
 * embi_clock_advance_fake moves it forward. The fake clock is process-wide, so tests that use it run one at a time.
 */
#include <stdbool.h>
#include <stdint.h>

/* Monotonic nanoseconds (the fake time when the fake clock is on). */
uint64_t embi_clock_ns(void);
/* Monotonic milliseconds with a fraction. */
double embi_clock_ms(void);
/* Turns the fake clock on at start_ns (on) or back to the real clock (off). */
void embi_clock_set_fake(bool on, uint64_t start_ns);
/* Moves the fake clock forward by ns. Does nothing when the fake clock is off. */
void embi_clock_advance_fake(uint64_t ns);
/* Whether the fake clock is on. */
bool embi_clock_is_fake(void);
