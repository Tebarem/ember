/*
 * ReadableStream core (stream.h): the queue with sizes and the high-water mark, delivery to a waiting read, deferred and
 * immediate close, error, cancel, consume-all, and the pull scheduling decision.
 */
#include "loader_test.h"

#include "loader/stream.h"

EMB_TEST(stream_enqueue_then_read_in_order)
{
    embi_stream s;
    embi_stream_init(&s, 3.0);
    s.started = true;
    EMB_CHECK(embi_stream_enqueue(&s, 1.0) == EMBI_STREAM_ENQ_QUEUED);
    EMB_CHECK(embi_stream_enqueue(&s, 2.0) == EMBI_STREAM_ENQ_QUEUED);
    EMB_CHECK(embi_stream_queued(&s) == 2);
    double d = 0;
    EMB_CHECK(embi_stream_desired_size(&s, &d) && d == 0.0);
    EMB_CHECK(!embi_stream_should_pull(&s));
    bool closed = true;
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_CHUNK && !closed);
    EMB_CHECK(embi_stream_desired_size(&s, &d) && d == 1.0);
    EMB_CHECK(embi_stream_should_pull(&s));
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_CHUNK);
    EMB_CHECK(embi_stream_queued(&s) == 0 && s.total == 0.0);
    EMB_CHECK(s.disturbed);
}

EMB_TEST(stream_pending_read_takes_the_next_chunk)
{
    embi_stream s;
    embi_stream_init(&s, 1.0);
    s.started = true;
    s.locked = true;
    bool closed;
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_PENDING);
    EMB_CHECK(s.nreads == 1);
    EMB_CHECK(embi_stream_should_pull(&s));
    EMB_CHECK(embi_stream_enqueue(&s, 1.0) == EMBI_STREAM_ENQ_DELIVERED);
    EMB_CHECK(s.nreads == 0 && embi_stream_queued(&s) == 0);
}

EMB_TEST(stream_close_deferred_until_drained)
{
    embi_stream s;
    embi_stream_init(&s, 1.0);
    EMB_CHECK(embi_stream_enqueue(&s, 1.0) == EMBI_STREAM_ENQ_QUEUED);
    EMB_CHECK(embi_stream_close(&s) == EMBI_STREAM_CLOSE_DEFERRED);
    EMB_CHECK(s.state == EMBI_STREAM_READABLE && s.close_requested);
    EMB_CHECK(embi_stream_enqueue(&s, 1.0) == EMBI_STREAM_ENQ_INVALID);
    EMB_CHECK(embi_stream_close(&s) == EMBI_STREAM_CLOSE_INVALID);
    bool closed = false;
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_CHUNK && closed);
    EMB_CHECK(s.state == EMBI_STREAM_CLOSED);
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_CLOSED);
    double d = 1.0;
    EMB_CHECK(embi_stream_desired_size(&s, &d) && d == 0.0);
}

EMB_TEST(stream_close_now_with_pending_reads)
{
    embi_stream s;
    embi_stream_init(&s, 1.0);
    bool closed;
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_PENDING);
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_PENDING);
    EMB_CHECK(embi_stream_close(&s) == EMBI_STREAM_CLOSE_NOW);
    EMB_CHECK(embi_stream_take_reads(&s) == 2);
    EMB_CHECK(embi_stream_take_reads(&s) == 0);
    EMB_CHECK(s.state == EMBI_STREAM_CLOSED);
}

EMB_TEST(stream_error_drops_queue_and_rejects_reads)
{
    embi_stream s;
    embi_stream_init(&s, 4.0);
    EMB_CHECK(embi_stream_enqueue(&s, 1.0) == EMBI_STREAM_ENQ_QUEUED);
    EMB_CHECK(embi_stream_error(&s));
    EMB_CHECK(!embi_stream_error(&s));
    EMB_CHECK(embi_stream_queued(&s) == 0 && s.total == 0.0);
    double d;
    EMB_CHECK(!embi_stream_desired_size(&s, &d));
    bool closed;
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_ERRORED);
    EMB_CHECK(embi_stream_enqueue(&s, 1.0) == EMBI_STREAM_ENQ_INVALID);
    EMB_CHECK(embi_stream_close(&s) == EMBI_STREAM_CLOSE_INVALID);
}

EMB_TEST(stream_cancel_and_consume_all)
{
    embi_stream s;
    embi_stream_init(&s, 4.0);
    EMB_CHECK(embi_stream_enqueue(&s, 2.0) == EMBI_STREAM_ENQ_QUEUED);
    EMB_CHECK(embi_stream_cancel(&s));
    EMB_CHECK(s.state == EMBI_STREAM_CLOSED && s.disturbed && embi_stream_queued(&s) == 0);
    EMB_CHECK(!embi_stream_cancel(&s));

    embi_stream t;
    embi_stream_init(&t, 4.0);
    EMB_CHECK(embi_stream_enqueue(&t, 2.0) == EMBI_STREAM_ENQ_QUEUED);
    embi_stream_consume_all(&t);
    EMB_CHECK(t.state == EMBI_STREAM_CLOSED && t.disturbed && embi_stream_queued(&t) == 0 && t.total == 0.0);
}

EMB_TEST(stream_should_pull_rules)
{
    embi_stream s;
    embi_stream_init(&s, 0.0);
    EMB_CHECK(!embi_stream_should_pull(&s)); /* not started */
    s.started = true;
    EMB_CHECK(!embi_stream_should_pull(&s)); /* desired size 0, no waiting read */
    s.locked = true;
    bool closed;
    EMB_CHECK(embi_stream_read(&s, 1.0, &closed) == EMBI_STREAM_READ_PENDING);
    EMB_CHECK(embi_stream_should_pull(&s)); /* a waiting read asks for a chunk */
    EMB_CHECK(embi_stream_close(&s) == EMBI_STREAM_CLOSE_NOW);
    EMB_CHECK(!embi_stream_should_pull(&s));
}

LOADER_TEST_MAIN()
