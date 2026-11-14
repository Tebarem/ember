/*
 * ReadableStream core (stream.h): the default stream's state machine.
 */
#include "loader/stream.h"

void embi_stream_init(embi_stream *s, double hwm)
{
    *s = (embi_stream){0};
    s->state = EMBI_STREAM_READABLE;
    s->hwm = hwm;
}

uint32_t embi_stream_queued(const embi_stream *s)
{
    return s->queued;
}

bool embi_stream_desired_size(const embi_stream *s, double *out)
{
    if (s->state == EMBI_STREAM_ERRORED) {
        return false;
    }
    *out = s->state == EMBI_STREAM_CLOSED ? 0.0 : s->hwm - s->total;
    return true;
}

bool embi_stream_can_enqueue(const embi_stream *s)
{
    return s->state == EMBI_STREAM_READABLE && !s->close_requested;
}

static void queue_reset(embi_stream *s)
{
    s->queued = 0;
    s->total = 0;
}

embi_stream_enq embi_stream_enqueue(embi_stream *s, double size)
{
    if (!embi_stream_can_enqueue(s)) {
        return EMBI_STREAM_ENQ_INVALID;
    }
    if (s->nreads > 0) {
        s->nreads--;
        return EMBI_STREAM_ENQ_DELIVERED;
    }
    s->queued++;
    s->total += size;
    return EMBI_STREAM_ENQ_QUEUED;
}

embi_stream_close_result embi_stream_close(embi_stream *s)
{
    if (!embi_stream_can_enqueue(s)) {
        return EMBI_STREAM_CLOSE_INVALID;
    }
    if (embi_stream_queued(s) > 0) {
        s->close_requested = true;
        return EMBI_STREAM_CLOSE_DEFERRED;
    }
    s->state = EMBI_STREAM_CLOSED;
    return EMBI_STREAM_CLOSE_NOW;
}

bool embi_stream_error(embi_stream *s)
{
    if (s->state != EMBI_STREAM_READABLE) {
        return false;
    }
    s->state = EMBI_STREAM_ERRORED;
    s->close_requested = false;
    queue_reset(s);
    return true;
}

uint32_t embi_stream_take_reads(embi_stream *s)
{
    uint32_t n = s->nreads;
    s->nreads = 0;
    return n;
}

embi_stream_read_result embi_stream_read(embi_stream *s, double head_size, bool *closed_now)
{
    *closed_now = false;
    s->disturbed = true;
    if (s->state == EMBI_STREAM_CLOSED) {
        return EMBI_STREAM_READ_CLOSED;
    }
    if (s->state == EMBI_STREAM_ERRORED) {
        return EMBI_STREAM_READ_ERRORED;
    }
    if (embi_stream_queued(s) > 0) {
        s->queued--;
        s->total -= head_size;
        if (s->queued == 0) {
            queue_reset(s);
            if (s->close_requested) {
                s->close_requested = false;
                s->state = EMBI_STREAM_CLOSED;
                *closed_now = true;
            }
        }
        return EMBI_STREAM_READ_CHUNK;
    }
    s->nreads++;
    return EMBI_STREAM_READ_PENDING;
}

bool embi_stream_cancel(embi_stream *s)
{
    s->disturbed = true;
    if (s->state != EMBI_STREAM_READABLE) {
        return false;
    }
    s->state = EMBI_STREAM_CLOSED;
    s->close_requested = false;
    queue_reset(s);
    return true;
}

void embi_stream_consume_all(embi_stream *s)
{
    s->disturbed = true;
    if (s->state == EMBI_STREAM_READABLE) {
        s->state = EMBI_STREAM_CLOSED;
        s->close_requested = false;
    }
    queue_reset(s);
}

bool embi_stream_should_pull(const embi_stream *s)
{
    if (!embi_stream_can_enqueue(s) || !s->started) {
        return false;
    }
    if (s->locked && s->nreads > 0) {
        return true;
    }
    double d;
    return embi_stream_desired_size(s, &d) && d > 0;
}
