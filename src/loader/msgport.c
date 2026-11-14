/*
 * MessagePort core (msgport.h).
 */
#include "loader/msgport.h"

typedef struct embi_mp_msg {
    struct embi_mp_msg *next;
    embi_sb bytes;
} embi_mp_msg;

struct embi_mport {
    uint32_t id;
    embi_mport *peer;
    bool closed;
    bool started;
    uint32_t slot;
    uint32_t nqueued;
    embi_mp_msg *head, *tail;
};

struct embi_mports {
    embi_hashmap map; /* id -> embi_mport * */
    uint32_t next_id;
};

embi_mports *embi_mports_new(void)
{
    embi_mports *r = embi_alloc(sizeof *r);
    *r = (embi_mports){0};
    embi_hashmap_init(&r->map);
    r->next_id = 1;
    return r;
}

static void queue_clear(embi_mport *p)
{
    while (p->head != nullptr) {
        embi_mp_msg *m = p->head;
        p->head = m->next;
        embi_sb_free(&m->bytes);
        embi_free(m, sizeof *m);
    }
    p->tail = nullptr;
    p->nqueued = 0;
}

void embi_mports_free(embi_mports *r)
{
    if (r == nullptr) {
        return;
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(&r->map);
    uint64_t k, v;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_mport *p = (embi_mport *)(uintptr_t)v;
        queue_clear(p);
        embi_free(p, sizeof *p);
    }
    embi_hashmap_free(&r->map);
    embi_free(r, sizeof *r);
}

static embi_mport *port_new(embi_mports *r)
{
    embi_mport *p = embi_alloc(sizeof *p);
    *p = (embi_mport){0};
    p->id = r->next_id++;
    embi_hashmap_put(&r->map, p->id, (uint64_t)(uintptr_t)p);
    return p;
}

embi_mport *embi_mp_channel(embi_mports *r, embi_mport **b)
{
    embi_mport *x = port_new(r);
    embi_mport *y = port_new(r);
    x->peer = y;
    y->peer = x;
    *b = y;
    return x;
}

embi_mport *embi_mp_new_closed(embi_mports *r)
{
    embi_mport *p = port_new(r);
    p->closed = true;
    return p;
}

void embi_mports_each(embi_mports *r, void (*fn)(embi_mport *p, void *arg), void *arg)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&r->map);
    uint64_t k, v;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        fn((embi_mport *)(uintptr_t)v, arg);
    }
}

embi_mport *embi_mp_find(embi_mports *r, uint32_t id)
{
    uint64_t v;
    return id != 0 && embi_hashmap_get(&r->map, id, &v) ? (embi_mport *)(uintptr_t)v : nullptr;
}

void embi_mp_release(embi_mports *r, embi_mport *p)
{
    if (!p->closed) {
        embi_mp_close(p);
    }
    embi_hashmap_remove(&r->map, p->id);
    queue_clear(p);
    embi_free(p, sizeof *p);
}

uint32_t embi_mp_id(const embi_mport *p)
{
    return p->id;
}

embi_mport *embi_mp_peer(const embi_mport *p)
{
    return p->peer;
}

bool embi_mp_is_closed(const embi_mport *p)
{
    return p->closed;
}

bool embi_mp_is_started(const embi_mport *p)
{
    return p->started;
}

uint32_t embi_mp_queued(const embi_mport *p)
{
    return p->nqueued;
}

uint32_t embi_mp_slot(const embi_mport *p)
{
    return p->slot;
}

void embi_mp_set_slot(embi_mport *p, uint32_t v)
{
    p->slot = v;
}

uint32_t embi_mp_start(embi_mport *p)
{
    if (p->closed || p->started) {
        return 0;
    }
    p->started = true;
    return p->nqueued;
}

void embi_mp_stop(embi_mport *p)
{
    p->started = false;
}

embi_mp_post_result embi_mp_post(embi_mport *from, embi_sb *msg, embi_mport **to)
{
    embi_mport *peer = from->peer;
    *to = peer;
    if (from->closed || peer == nullptr || peer->closed) {
        embi_sb_free(msg);
        return EMBI_MP_DROPPED;
    }
    embi_mp_msg *m = embi_alloc(sizeof *m);
    m->next = nullptr;
    m->bytes = *msg;
    *msg = (embi_sb){0};
    if (peer->tail != nullptr) {
        peer->tail->next = m;
    } else {
        peer->head = m;
    }
    peer->tail = m;
    peer->nqueued++;
    return peer->started ? EMBI_MP_SCHEDULE : EMBI_MP_QUEUED;
}

bool embi_mp_take(embi_mport *p, embi_sb *msg)
{
    if (p->closed || !p->started || p->head == nullptr) {
        return false;
    }
    embi_mp_msg *m = p->head;
    p->head = m->next;
    if (p->head == nullptr) {
        p->tail = nullptr;
    }
    p->nqueued--;
    *msg = m->bytes;
    embi_free(m, sizeof *m);
    return true;
}

void embi_mp_close(embi_mport *p)
{
    if (p->closed) {
        return;
    }
    p->closed = true;
    queue_clear(p);
    if (p->peer != nullptr) {
        p->peer->peer = nullptr;
        p->peer = nullptr;
    }
}
