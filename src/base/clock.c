/*
 * The monotonic clock (W5c). The real clock is SDL's nanosecond tick counter; the fake clock is for tests.
 */
#include "base/clock.h"

#include <SDL3/SDL_timer.h>

static bool s_fake;
static uint64_t s_fake_ns;

uint64_t embi_clock_ns(void)
{
    return s_fake ? s_fake_ns : SDL_GetTicksNS();
}

double embi_clock_ms(void)
{
    return (double)embi_clock_ns() / 1e6;
}

void embi_clock_set_fake(bool on, uint64_t start_ns)
{
    s_fake = on;
    s_fake_ns = start_ns;
}

void embi_clock_advance_fake(uint64_t ns)
{
    if (s_fake) {
        s_fake_ns += ns;
    }
}

bool embi_clock_is_fake(void)
{
    return s_fake;
}
