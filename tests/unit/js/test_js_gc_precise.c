/* Precise roots of the collector (J5m1). The conservative
 * stack scan starts at the frame of the collection entry (gc.c collect_entry) and reads every frame
 * above it. The cases here hold values in a persistent handle and drop unrooted twins, then require a
 * single collection to keep the rooted cells and free the others.
 *
 * Cell pointers that a finished call leaves in its frames stay on the stack until something overwrites
 * them, and a later collection at the same depth scans them. So the allocating and inspecting steps
 * run on helper threads: their stacks are dead once they exit and the collection never reads them. The
 * main thread only calls the collector and the stack scrub. */
#include "emb_test.h"

#include "js/js.h"

#include <string.h>

#include <SDL3/SDL_thread.h>

#define PRECISE_NOINLINE __attribute__((noinline))

/* A cell kind with one value field that the trace hook marks. The tag in the field says which cell
 * was finalized; it is stored in the SHAPE type slot, which the test runtime uses for this kind. */
typedef struct precise_cell {
    embi_js_cell hdr;
    embi_js_value payload;
} precise_cell;

/* Bit t set when the cell tagged t was finalized. */
static uint32_t g_dead_tags;

static void precise_trace(embi_js_cell *c, embi_js_tracer *t)
{
    embi_js_mark_value(t, ((precise_cell *)c)->payload);
}

static void precise_finalize(embi_js_cell *c)
{
    uint32_t tag = (uint32_t)embi_js_as_double(((precise_cell *)c)->payload);
    if (tag < 32u) {
        g_dead_tags |= 1u << tag;
    }
}

static void precise_install(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){precise_trace, precise_finalize});
}

static PRECISE_NOINLINE embi_js_value precise_new(embi_js_runtime *rt, uint32_t tag)
{
    precise_cell *c = (precise_cell *)embi_js_alloc_cell(rt, sizeof *c, EMBI_JS_CELL_SHAPE);
    c->payload = embi_js_number((double)tag);
    return embi_js_from_cell(c);
}

static PRECISE_NOINLINE void scrub_stack_precise(void)
{
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

/* Runs fn(arg) on a helper thread and waits for it. */
typedef struct job {
    void (*fn)(struct job *);
    embi_js_runtime *rt;
    uint32_t tag;
    uint32_t id;
    int result;
} job;

static int job_thread(void *data)
{
    job *j = data;
    j->fn(j);
    return 0;
}

static void run_on_thread(job *j)
{
    SDL_Thread *t = SDL_CreateThread(job_thread, "precise", j);
    EMB_REQUIRE(t != nullptr);
    if (t) {
        SDL_WaitThread(t, nullptr);
    }
}

/* Allocates a rooted cell: its only reference is the persistent handle. */
static void job_make_rooted(job *j)
{
    j->id = embi_js_persistent_new(j->rt, precise_new(j->rt, j->tag));
}

/* Allocates an unrooted twin of the same kind and drops it. */
static void job_make_unrooted(job *j)
{
    (void)precise_new(j->rt, j->tag);
}

/* Reads the payload tag of the cell behind a handle: 1 when the cell is alive and holds tag. */
static void job_rooted_payload_is(job *j)
{
    embi_js_value kept = embi_js_persistent_get(j->rt, j->id);
    j->result = 0;
    if (embi_js_is_cell(kept)) {
        j->result = (int)embi_js_as_double(((precise_cell *)embi_js_as_cell(kept))->payload) == (int)j->tag;
    }
}

/* A value held only by a persistent handle survives one collection, and an unrooted twin made in the
 * same way dies in that same collection. */
static void rooted_survives_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    precise_install(rt);
    g_dead_tags = 0;

    job make = {job_make_rooted, rt, 1, 0, 0};
    run_on_thread(&make);
    job twin = {job_make_unrooted, rt, 2, 0, 0};
    run_on_thread(&twin);
    scrub_stack_precise();
    embi_js_gc(rt);

    EMB_CHECK((g_dead_tags & (1u << 2)) != 0);
    EMB_CHECK((g_dead_tags & (1u << 1)) == 0);
    job check = {job_rooted_payload_is, rt, 1, make.id, 0};
    run_on_thread(&check);
    EMB_CHECK(check.result == 1);

    /* Once the handle is freed the value dies at the next collection. */
    embi_js_persistent_free(rt, make.id);
    scrub_stack_precise();
    embi_js_gc(rt);
    EMB_CHECK((g_dead_tags & (1u << 1)) != 0);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_precise_persistent_root_survives_and_unrooted_twin_dies)
{
    rooted_survives_body();
}

/* An unrooted cell whose only reference is a local that stays live across the collection. At -O1 and
 * above the compiler keeps such a local in a callee-saved register, so the collector finds it only
 * through the registers that collect_entry's prologue saves (gc.c, collect_entry). The cell must survive
 * the collection and still read back its tag. Which register holds the local depends on the compiler
 * and the optimization level. */
static PRECISE_NOINLINE int callee_saved_body(embi_js_runtime *rt)
{
    embi_js_value v = precise_new(rt, 3);
    scrub_stack_precise();
    embi_js_gc(rt);
    int alive = !(g_dead_tags & (1u << 3));
    alive = alive && embi_js_is_cell(v) &&
            (int)embi_js_as_double(((precise_cell *)embi_js_as_cell(v))->payload) == 3;
    return alive;
}

EMB_TEST(gc_precise_callee_saved_register_of_caller_is_scanned)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    precise_install(rt);
    g_dead_tags = 0;
    EMB_CHECK(callee_saved_body(rt) != 0);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
