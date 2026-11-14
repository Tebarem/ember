#pragma once
/*
 * MessagePort core (W7f): entangled pairs of ports, each with a queue of serialized messages and the
 * "started" flag of the port message queue. C only, no JS header: a message is the byte string of a structured
 * serialization (clone.h), so the core is tested with plain bytes. The bindings (src/bindings/impl_msgport.c) own the
 * delivery: they post one task per message once the receiving port is started, take the message off the queue in the
 * task and deserialize it.
 *
 * The registry belongs to one document (one view's thread); nothing here locks. Ports are freed with the registry, or earlier by embi_mp_release.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_mport embi_mport;
typedef struct embi_mports embi_mports;

embi_mports *embi_mports_new(void);
/* Frees every port and every message still queued. */
void embi_mports_free(embi_mports *r);
/* A registry shared by several documents (IF1: the frames of one view, which run on one thread): each user takes a
 * reference, and the last embi_mports_unref frees it (a new registry has one reference). */
void embi_mports_retain(embi_mports *r);
void embi_mports_unref(embi_mports *r);

/* A new entangled pair: returns the first port and stores the second in *b. Ids are 1, 2, 3 ... in creation order. */
embi_mport *embi_mp_channel(embi_mports *r, embi_mport **b);
/* Calls fn for every port of the registry. */
void embi_mports_each(embi_mports *r, void (*fn)(embi_mport *p, void *arg), void *arg);
/* The port with the id (0 and unknown ids give nullptr). */
embi_mport *embi_mp_find(embi_mports *r, uint32_t id);
/* A fresh, closed, disentangled port record: stands in for a closed port that is transferred (the HTML standard allows
 * it). A record whose id travels in a message that is later dropped stays in the registry until embi_mports_free. */
embi_mport *embi_mp_new_closed(embi_mports *r);

uint32_t embi_mp_id(const embi_mport *p);
/* The entangled port, or nullptr once either side has been closed. */
embi_mport *embi_mp_peer(const embi_mport *p);
bool embi_mp_is_closed(const embi_mport *p);
bool embi_mp_is_started(const embi_mport *p);
uint32_t embi_mp_queued(const embi_mport *p);
/* The binding's slot (the persistent handle of the port's wrapper plus one; 0: none). */
uint32_t embi_mp_slot(const embi_mport *p);
void embi_mp_set_slot(embi_mport *p, uint32_t v);
/* The binding context the port belongs to in a shared registry (its wrapper's realm; nullptr: none, or in transit). */
void *embi_mp_owner(const embi_mport *p);
void embi_mp_set_owner(embi_mport *p, void *owner);

/* Enables the port message queue. Returns the number of messages queued at that moment (each needs a delivery task), 0 when
 * the port was started already or is closed. */
uint32_t embi_mp_start(embi_mport *p);
/* Turns the queue off again (a port that is transferred away starts a new life disabled). */
void embi_mp_stop(embi_mport *p);

typedef enum embi_mp_post_result {
    EMBI_MP_DROPPED, /* no peer or the sender is closed: the message is discarded */
    EMBI_MP_QUEUED,  /* queued on the peer, which is not started: no task yet */
    EMBI_MP_SCHEDULE /* queued on the peer, which is started: the caller posts one delivery task for the peer */
} embi_mp_post_result;
/* Queues msg (its buffer is taken over: *msg is zeroed) on the peer of from. The peer is returned in *to. */
embi_mp_post_result embi_mp_post(embi_mport *from, embi_sb *msg, embi_mport **to);
/* Takes the oldest message of an open, started port (the caller frees the buffer). False when there is none. */
bool embi_mp_take(embi_mport *p, embi_sb *msg);
/* Closes the port: drops its queue, disentangles it and its peer (the peer stays open but has nobody to talk to). */
void embi_mp_close(embi_mport *p);
/* Closes the port if open, then unlinks and frees it: embi_mp_find no longer finds its id. For a port whose wrapper is done with
 * it (MessagePort.close()); the caller must have dropped the binding slot. */
void embi_mp_release(embi_mports *r, embi_mport *p);
