/*
 * The monotonic clock (W5c): the real clock never goes back, the fake clock freezes and advances on request, and the
 * millisecond reading is the nanosecond one divided by 1e6. No GPU and no window needed.
 */
#include "emb_test.h"

#include "base/clock.h"

EMB_TEST(clock_real_is_monotonic)
{
    embi_clock_set_fake(false, 0);
    EMB_CHECK(!embi_clock_is_fake());
    uint64_t a = embi_clock_ns();
    uint64_t b = embi_clock_ns();
    EMB_CHECK(b >= a);
    double ms = embi_clock_ms();
    EMB_CHECK(ms >= 0.0);
}

EMB_TEST(clock_fake_freezes_and_advances)
{
    embi_clock_set_fake(true, 5000000000ULL); /* 5 s */
    EMB_CHECK(embi_clock_is_fake());
    EMB_CHECK(embi_clock_ns() == 5000000000ULL);
    EMB_CHECK(embi_clock_ns() == 5000000000ULL);
    EMB_CHECK(embi_clock_ms() == 5000.0);

    embi_clock_advance_fake(1500000ULL); /* 1.5 ms */
    EMB_CHECK(embi_clock_ns() == 5001500000ULL);
    EMB_CHECK(embi_clock_ms() == 5001.5);

    embi_clock_set_fake(false, 0);
    EMB_CHECK(!embi_clock_is_fake());
}

EMB_TEST(clock_advance_is_ignored_on_the_real_clock)
{
    embi_clock_set_fake(false, 0);
    uint64_t before = embi_clock_ns();
    embi_clock_advance_fake(1000000000ULL);
    uint64_t after = embi_clock_ns();
    /* The real clock moved only by the time that passed, not by the 1 s advance. */
    EMB_CHECK(after - before < 1000000000ULL);
}

EMB_TEST_MAIN()
