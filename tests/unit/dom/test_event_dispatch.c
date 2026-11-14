/*
 * The event dispatch algorithm (src/dom/event.c) on a fake tree: the order of the capturing, target
 * and bubbling passes, stopPropagation, stopImmediatePropagation, preventDefault with cancelable and
 * passive listeners, the state reset at the end of a dispatch, and long paths.
 */
#include "emb_test.h"

#include "dom/event.h"

#include <string.h>

#define MAX_NODES 64
#define MAX_LOG 256

typedef struct fnode {
    int id;
    int parent; /* index, -1 for the root */
    int listeners; /* listeners this node runs for the event */
    bool act;      /* has an activation behavior */
} fnode;

typedef struct logrec {
    int id;
    bool capture;
    uint8_t phase;
} logrec;

typedef struct world {
    fnode nodes[MAX_NODES];
    int count;
    logrec log[MAX_LOG];
    int nlog;
    int stop_prop_at;   /* stopPropagation from this node's first listener, -1 for none */
    int stop_imm_at;    /* stopImmediatePropagation from this node's first listener */
    int prevent_at;     /* preventDefault from this node's first listener */
    int passive_at;     /* run this node's listeners as passive (preventDefault has no effect) */
    int seen_target;    /* ev->target.obj index seen by the listeners */
    int seen_current;   /* last ev->current.obj index seen */
    char alog[32];      /* activation calls: "<id><p|r|c>" for pre, run, canceled */
    int nalog;
    int bad_path;       /* listeners that saw a path or phase inconsistent with the item */
} world;

static embi_event_item item_of(world *w, int index)
{
    return (embi_event_item){&w->nodes[index], 0};
}

static int index_of(const void *obj, const world *w)
{
    return (int)(((const fnode *)obj) - w->nodes);
}

static embi_event_item op_parent(void *ctx, embi_event_item item, const embi_event *ev)
{
    world *w = ctx;
    (void)ev;
    int p = ((fnode *)item.obj)->parent;
    if (p < 0) {
        return (embi_event_item){nullptr, 0};
    }
    return item_of(w, p);
}

static void op_invoke(void *ctx, embi_event_item item, embi_event *ev, bool capture)
{
    world *w = ctx;
    int id = index_of(item.obj, w);
    if (ev->current.obj != item.obj || ev->target.obj == nullptr) {
        w->bad_path++;
    }
    w->seen_current = id;
    w->seen_target = index_of(ev->target.obj, w);
    for (int k = 0; k < w->nodes[id].listeners; k++) {
        if (ev->flags & EMBI_EVF_STOP_IMMEDIATE) {
            break;
        }
        if (w->nlog < MAX_LOG) {
            w->log[w->nlog++] = (logrec){id, capture, ev->phase};
        }
        bool passive = w->passive_at == id;
        if (passive) {
            ev->flags |= EMBI_EVF_IN_PASSIVE;
        }
        if (k == 0 && id == w->stop_prop_at) {
            embi_event_stop_propagation(ev);
        }
        if (k == 0 && id == w->stop_imm_at) {
            embi_event_stop_immediate(ev);
        }
        if (k == 0 && id == w->prevent_at) {
            embi_event_prevent_default(ev);
        }
        if (passive) {
            ev->flags &= ~EMBI_EVF_IN_PASSIVE;
        }
    }
}

static bool op_has_activation(void *ctx, embi_event_item item, const embi_event *ev)
{
    (void)ctx;
    (void)ev;
    return ((fnode *)item.obj)->act;
}

static void op_activation(void *ctx, embi_event_item item, embi_event *ev, int phase)
{
    world *w = ctx;
    (void)ev;
    if (w->nalog < 28) {
        w->alog[w->nalog++] = (char)('0' + index_of(item.obj, w));
        w->alog[w->nalog++] = phase == EMBI_ACT_PRE ? 'p' : (phase == EMBI_ACT_RUN ? 'r' : 'c');
    }
    if (phase == EMBI_ACT_PRE && w->nalog < 28) {
        w->alog[w->nalog++] = ev->phase == EMBI_PHASE_NONE ? 'n' : 'x'; /* before the passes: phase still none */
    }
}

static embi_event_ops ops_of(world *w)
{
    (void)w;
    return (embi_event_ops){nullptr, op_parent, op_invoke, op_has_activation, op_activation};
}

/* Builds a chain: node 0 is the root, node n-1 the deepest, each listens `listeners` times. */
static void chain(world *w, int n, int listeners)
{
    memset(w, 0, sizeof *w);
    w->stop_prop_at = w->stop_imm_at = w->prevent_at = w->passive_at = -1;
    w->count = n;
    for (int i = 0; i < n; i++) {
        w->nodes[i] = (fnode){i, i - 1, listeners, false};
    }
}

static bool dispatch(world *w, embi_event *ev, int target)
{
    embi_event_ops ops = ops_of(w);
    ops.ctx = w;
    return embi_event_dispatch(&ops, ev, item_of(w, target));
}

static embi_event *event(const char *type, uint32_t flags)
{
    return embi_event_new((embi_str){type, (uint32_t)strlen(type)}, flags, 0.0);
}

/* The log as a string of "<id><c|t|b>" entries, c for capture, t for at target, b for bubble. */
static bool log_is(const world *w, const char *want)
{
    char got[MAX_LOG * 4] = {0};
    size_t n = 0;
    for (int i = 0; i < w->nlog; i++) {
        const logrec *r = &w->log[i];
        char tag = r->phase == EMBI_PHASE_AT_TARGET ? 't' : (r->capture ? 'c' : 'b');
        n += (size_t)SDL_snprintf(got + n, sizeof got - n, "%s%d%c", i ? " " : "", r->id, tag);
    }
    if (strcmp(got, want) != 0) {
        SDL_Log("    log: got \"%s\" want \"%s\"", got, want);
        return false;
    }
    return true;
}

EMB_TEST(event_order_capture_target_bubble)
{
    world w;
    chain(&w, 4, 1);
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 3));
    EMB_CHECK(log_is(&w, "0c 1c 2c 3t 3t 2b 1b 0b"));
    EMB_CHECK_EQ_INT(w.bad_path, 0);
    embi_event_free(ev);
}

EMB_TEST(event_order_non_bubbling_skips_ancestors)
{
    world w;
    chain(&w, 4, 1);
    embi_event *ev = event("focus", 0);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 3));
    /* the target still runs in the bubbling pass; ancestors do not */
    EMB_CHECK(log_is(&w, "0c 1c 2c 3t 3t"));
    embi_event_free(ev);
}

EMB_TEST(event_stop_propagation_in_capture_pass)
{
    world w;
    chain(&w, 4, 1);
    w.stop_prop_at = 1;
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 3));
    EMB_CHECK(log_is(&w, "0c 1c"));
    embi_event_free(ev);
}

EMB_TEST(event_stop_propagation_at_target_keeps_target_bubble_out)
{
    world w;
    chain(&w, 4, 2);
    w.stop_prop_at = 3;
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 3));
    /* the target's two capture listeners run; stopping in the capture pass stops the bubble pass */
    EMB_CHECK(log_is(&w, "0c 0c 1c 1c 2c 2c 3t 3t"));
    embi_event_free(ev);
}

EMB_TEST(event_stop_immediate_skips_the_rest_of_the_item)
{
    world w;
    chain(&w, 3, 3);
    w.stop_imm_at = 2;
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 2));
    /* the target's first capture listener stops immediately: its other two listeners do not run,
     * and stopping propagation ends the dispatch before the bubbling pass */
    EMB_CHECK(log_is(&w, "0c 0c 0c 1c 1c 1c 2t"));
    embi_event_free(ev);
}

EMB_TEST(event_stop_immediate_stops_later_items_too)
{
    world w;
    chain(&w, 3, 2);
    w.stop_imm_at = 1;
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 2));
    /* stopImmediatePropagation sets the propagation flag as well: nothing else runs */
    EMB_CHECK(log_is(&w, "0c 0c 1c"));
    embi_event_free(ev);
}

EMB_TEST(event_prevent_default_needs_cancelable)
{
    world w;
    chain(&w, 2, 1);
    w.prevent_at = 0;
    embi_event *ev = event("click", EMBI_EVF_CANCELABLE | EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 1) == false);
    EMB_CHECK(ev->flags & EMBI_EVF_CANCELED);
    embi_event_free(ev);

    chain(&w, 2, 1);
    w.prevent_at = 0;
    ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 1));
    EMB_CHECK((ev->flags & EMBI_EVF_CANCELED) == 0);
    embi_event_free(ev);
}

EMB_TEST(event_passive_listener_cannot_cancel)
{
    world w;
    chain(&w, 2, 1);
    w.prevent_at = 0;
    w.passive_at = 0;
    embi_event *ev = event("wheel", EMBI_EVF_CANCELABLE | EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 1));
    embi_event_free(ev);
}

EMB_TEST(event_state_is_reset_after_dispatch)
{
    world w;
    chain(&w, 3, 1);
    w.stop_prop_at = 0;
    embi_event *ev = event("click", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 2));
    EMB_CHECK((ev->flags & (EMBI_EVF_DISPATCHING | EMBI_EVF_STOP_PROPAGATION | EMBI_EVF_STOP_IMMEDIATE)) == 0);
    EMB_CHECK_EQ_INT(ev->phase, EMBI_PHASE_NONE);
    EMB_CHECK(ev->current.obj == nullptr);
    EMB_CHECK(ev->path == nullptr);
    EMB_CHECK(ev->target.obj == &w.nodes[2]);
    /* a second dispatch runs again */
    w.stop_prop_at = -1;
    w.nlog = 0;
    EMB_CHECK(dispatch(&w, ev, 2));
    EMB_CHECK(log_is(&w, "0c 1c 2t 2t 1b 0b"));
    embi_event_free(ev);
}

EMB_TEST(event_long_path_grows_the_path)
{
    world w;
    chain(&w, MAX_NODES, 1);
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, MAX_NODES - 1));
    /* capture: 63 ancestors, target twice, bubble: 63 ancestors */
    EMB_CHECK_EQ_INT(w.nlog, 2 * (MAX_NODES - 1) + 2);
    EMB_CHECK_EQ_INT(w.bad_path, 0);
    embi_event_free(ev);
}

EMB_TEST(event_retype_keeps_flags_and_clears_dispatch_state)
{
    embi_event *ev = event("a", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED | EMBI_EVF_TRUSTED);
    EMB_REQUIRE(ev);
    ev->flags |= EMBI_EVF_STOP_PROPAGATION;
    embi_event *n = embi_event_retype(ev, (embi_str){"change", 6});
    EMB_REQUIRE(n);
    EMB_CHECK(embi_event_type(n).len == 6);
    EMB_CHECK(memcmp(n->type, "change", 6) == 0);
    EMB_CHECK(n->flags & EMBI_EVF_BUBBLES);
    EMB_CHECK(n->flags & EMBI_EVF_TRUSTED);
    EMB_CHECK((n->flags & EMBI_EVF_STOP_PROPAGATION) == 0);
    EMB_CHECK(embi_event_size(n) == sizeof(embi_event) + 6);
    embi_event_free(ev);
    embi_event_free(n);
}

EMB_TEST(event_activation_on_target)
{
    world w;
    chain(&w, 3, 1);
    w.nodes[2].act = true;
    w.nodes[1].act = true; /* the target wins over the ancestor */
    embi_event *ev = event("click", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 2));
    EMB_CHECK_EQ_INT(strcmp(w.alog, "2pn2r"), 0);
    embi_event_free(ev);
}

EMB_TEST(event_activation_on_bubbling_ancestor)
{
    world w;
    chain(&w, 4, 0);
    w.nodes[1].act = true;
    w.nodes[0].act = true; /* only the nearest one */
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 3));
    EMB_CHECK_EQ_INT(strcmp(w.alog, "1pn1r"), 0);
    embi_event_free(ev);
    /* a non-bubbling event does not reach the ancestor's behavior */
    chain(&w, 4, 0);
    w.nodes[1].act = true;
    ev = event("click", 0);
    EMB_REQUIRE(ev);
    EMB_CHECK(dispatch(&w, ev, 3));
    EMB_CHECK_EQ_INT(w.nalog, 0);
    embi_event_free(ev);
}

EMB_TEST(event_activation_canceled_runs_the_cancel_hook)
{
    world w;
    chain(&w, 3, 1);
    w.nodes[2].act = true;
    w.prevent_at = 1;
    embi_event *ev = event("click", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE);
    EMB_REQUIRE(ev);
    EMB_CHECK(!dispatch(&w, ev, 2));
    EMB_CHECK_EQ_INT(strcmp(w.alog, "2pn2c"), 0);
    embi_event_free(ev);
}

EMB_TEST(event_no_activation_hooks_is_fine)
{
    world w;
    chain(&w, 2, 1);
    embi_event_ops ops = {&w, op_parent, op_invoke, nullptr, nullptr};
    embi_event *ev = event("click", EMBI_EVF_BUBBLES);
    EMB_REQUIRE(ev);
    EMB_CHECK(embi_event_dispatch(&ops, ev, item_of(&w, 1)));
    embi_event_free(ev);
}

EMB_TEST_MAIN()
