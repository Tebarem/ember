#pragma once
/*
 * ReadableStream core (W7c): the state machine of the WHATWG default stream (readable,
 * closed and errored states, the chunk queue with its total size and high-water mark, the pending read count, the
 * close request, the lock and disturbed flags, and the pull scheduling flags). C only: no JS header. The chunk values
 * live in the JS binding (src/bindings/impl_loader_stream.c), which keeps a JS array in step with this queue; the core
 * tracks only the number of chunks and their total size, so it holds no heap memory (a wrapper's finalizer frees only its
 * flat block) and can be tested with plain numbers. The caller passes the size of the chunk it takes off the queue.
 *
 * Single-threaded: one stream belongs to one view's thread. Internal API: embi_ prefix.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum embi_stream_state {
    EMBI_STREAM_READABLE,
    EMBI_STREAM_CLOSED,
    EMBI_STREAM_ERRORED,
} embi_stream_state;

typedef struct embi_stream {
    embi_stream_state state;
    bool locked;          /* a reader holds the stream */
    bool disturbed;       /* a read or a cancel happened */
    bool close_requested; /* close() came while chunks were still queued */
    bool started;         /* the underlying source's start finished */
    bool pulling;         /* a pull call is in flight */
    bool pull_again;      /* a pull was asked for while one was in flight */
    double hwm;           /* the high-water mark */
    double total;         /* the sum of the queued chunk sizes */
    uint32_t nreads;      /* read requests waiting for a chunk */
    uint32_t queued;      /* the number of queued chunks */
} embi_stream;

/* A readable stream with the given high-water mark (not negative, not NaN: the caller checks). */
void embi_stream_init(embi_stream *s, double hwm);

/* The number of queued chunks. */
uint32_t embi_stream_queued(const embi_stream *s);
/* ReadableStreamDefaultControllerGetDesiredSize: hwm - total while readable, 0 when closed. Returns false for an
 * errored stream (the desiredSize attribute is then null). */
bool embi_stream_desired_size(const embi_stream *s, double *out);
/* CanCloseOrEnqueue: readable and no close requested. */
bool embi_stream_can_enqueue(const embi_stream *s);

typedef enum embi_stream_enq {
    EMBI_STREAM_ENQ_INVALID,   /* closed, close requested or errored: the controller throws TypeError */
    EMBI_STREAM_ENQ_DELIVERED, /* a waiting read takes the chunk at once: resolve the oldest read, queue nothing */
    EMBI_STREAM_ENQ_QUEUED,    /* the chunk is queued (the caller appends its value to its own queue) */
} embi_stream_enq;
embi_stream_enq embi_stream_enqueue(embi_stream *s, double size);

typedef enum embi_stream_close_result {
    EMBI_STREAM_CLOSE_INVALID,  /* not readable, or a close was requested already */
    EMBI_STREAM_CLOSE_NOW,      /* the stream is closed now: settle the pending reads (embi_stream_take_reads) with done */
    EMBI_STREAM_CLOSE_DEFERRED, /* chunks are queued: the stream closes when the last one is read */
} embi_stream_close_result;
embi_stream_close_result embi_stream_close(embi_stream *s);
/* Errors the stream (controller error() and failed pulls): no effect unless readable. Returns true when the state
 * changed. The queue is dropped; the pending reads are for the caller to reject (embi_stream_take_reads). */
bool embi_stream_error(embi_stream *s);
/* Returns the number of pending reads and sets it to zero. */
uint32_t embi_stream_take_reads(embi_stream *s);

typedef enum embi_stream_read_result {
    EMBI_STREAM_READ_CHUNK,   /* a chunk was dequeued (the caller shifts its own queue); *closed_now is set when it was the
                                 last chunk of a requested close and the stream is closed now */
    EMBI_STREAM_READ_PENDING, /* nothing queued: the read waits (nreads was incremented) */
    EMBI_STREAM_READ_CLOSED,  /* closed and drained: resolve with done */
    EMBI_STREAM_READ_ERRORED, /* reject with the stored error */
} embi_stream_read_result;
/* head_size is the size of the oldest queued chunk (ignored when nothing is queued). */
embi_stream_read_result embi_stream_read(embi_stream *s, double head_size, bool *closed_now);

/* ReadableStreamCancel without the source call: marks the stream disturbed and, when readable, closes it and drops the
 * queue. Returns true when it was readable (the pending reads, if any, are for the caller to settle). */
bool embi_stream_cancel(embi_stream *s);
/* A stream whose chunks were consumed in one go (body consumption): disturbed, closed, queue dropped. */
void embi_stream_consume_all(embi_stream *s);

/* ReadableStreamDefaultControllerShouldCallPull. */
bool embi_stream_should_pull(const embi_stream *s);
