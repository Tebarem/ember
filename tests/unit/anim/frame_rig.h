#pragma once
/*
 * A rig for the frame tests (W6c): a document with scripts on a fake clock and no view (no GPU). frame() is one rendering
 * opportunity as the view does it: the frame step, the style and layout update, the end of the frame. js() evaluates a
 * script and keeps the result text; the console and the script log are captured.
 */
#include "emb_test.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "base/clock.h"
#include "embed/embed_int.h"
#include "embed/frame.h"
#include "embed/script_int.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

typedef struct rig_buf {
    char text[4096];
    size_t len;
} rig_buf;

typedef struct rig {
    emb_runtime *rt;
    embi_doc *d;
    rig_buf out; /* console output, one entry per line, ';' separated */
    rig_buf err; /* the script log */
    char res[512];
    bool ok;
} rig;

static void rig_add(rig_buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 < sizeof b->text) {
        memcpy(b->text + b->len, s, n);
        b->len += n;
        b->text[b->len++] = ';';
        b->text[b->len] = '\0';
    }
}

static void rig_console(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)level;
    rig_add(&((rig *)user)->out, text, len);
}

static void rig_log(void *user, emb_log_level level, const char *text)
{
    (void)level;
    rig_add(&((rig *)user)->err, text, strlen(text));
}

static void rig_setup(rig *r)
{
    memset(r, 0, sizeof *r);
    embi_clock_set_fake(true, 1000000000ULL);
    r->rt = emb_runtime_create(nullptr);
    embi_doc_cb cb = {r, nullptr, nullptr, nullptr};
    r->d = embi_doc_create(r->rt->text, r->rt->fonts, &cb);
    embi_script_set_console(embi_doc_script(r->d), rig_console, r);
    embi_script_set_log(embi_doc_script(r->d), rig_log, r);
}

static void rig_teardown(rig *r)
{
    embi_doc_destroy(r->d);
    emb_runtime_destroy(r->rt);
    embi_clock_set_fake(false, 0);
    if (embi_arena_debug_poison_on()) {
        embi_arena_debug_poison(false); /* frees what the poison mode kept aside */
    }
}

/* WPTASAN: rig_setup with the poison mode of the arenas on (base.h, embi_arena_debug_poison): memory an arena gives back is
 * poisoned and never reused, so a style that outlives the memory of its values is caught by rig_poison, without ASan. */
static void rig_setup_poisoned(rig *r)
{
    embi_arena_debug_poison(true);
    rig_setup(r);
}

static uint32_t rig_poison_tree(const embi_node *n)
{
    uint32_t k = 0;
    for (const embi_node *c = n->first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            k += embi_style_debug_poisoned((const embi_style *)((const embi_element *)c)->style);
            k += rig_poison_tree(c);
        }
    }
    return k;
}

/* The values in poisoned memory that the document can reach: its elements' styles, the styles the animator keeps (base and
 * parent styles), and the old and parent styles that the restyles so far have read. 0 when every style's values are alive. */
static uint32_t rig_poison(rig *r)
{
    uint32_t k = embi_style_debug_poison_reads(embi_doc_style_engine(r->d));
    k += embi_animator_debug_poisoned(embi_doc_animator(r->d));
    embi_document *dom = embi_doc_dom(r->d);
    if (dom != nullptr) {
        k += rig_poison_tree(&dom->base);
    }
    return k;
}

static void rig_load(rig *r, const char *html)
{
    (void)embi_doc_load(r->d, (embi_str){html, (uint32_t)strlen(html)}, (embi_str){"http://t.test/page.html", 23}, nullptr);
}

static void rig_result(void *user, bool ok, const char *text, size_t len)
{
    rig *r = user;
    r->ok = ok;
    size_t n = len < sizeof r->res - 1 ? len : sizeof r->res - 1;
    memcpy(r->res, text, n);
    r->res[n] = '\0';
}

/* Evaluates src; the result text is in r->res. */
static bool rig_js(rig *r, const char *src)
{
    return embi_script_eval(embi_doc_script(r->d), src, strlen(src), rig_result, r);
}

static void rig_ms(uint64_t n)
{
    embi_clock_advance_fake(n * 1000000ULL);
}

/* One frame at the current clock; returns whether the update rebuilt the fragments. */
static bool rig_frame(rig *r)
{
    embi_doc_frame(r->d);
    embi_doc_times tm;
    memset(&tm, 0, sizeof tm);
    bool rebuilt = embi_doc_update(r->d, &tm);
    embi_doc_frame_end(r->d);
    return rebuilt;
}

/* Advances the clock by step ms and runs a frame, n times. */
static void rig_frames(rig *r, uint32_t n, uint64_t step)
{
    for (uint32_t i = 0; i < n; i++) {
        rig_ms(step);
        (void)rig_frame(r);
    }
}
