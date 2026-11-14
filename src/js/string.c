#include "js/runtime.h"
#include "js/string.h"

#include "num/num.h"

/* ------------------------------------------------------------------ raw access */

static uint8_t *latin1_mut(embi_js_string *s)
{
    return (uint8_t *)s + sizeof(embi_js_string);
}

static uint16_t *wide_mut(embi_js_string *s)
{
    return (uint16_t *)((uint8_t *)s + sizeof(embi_js_string));
}

static embi_js_string *alloc_string(embi_js_runtime *rt, size_t len, bool wide)
{
    if (len > EMBI_JS_STR_MAX_LEN) {
        embi_log_error("string length %llu exceeds the engine limit",
                       (unsigned long long)len);
        abort();
    }
    size_t bytes = sizeof(embi_js_string) + (wide ? 2u : 1u) * (size_t)len;
    embi_js_string *s = (embi_js_string *)embi_js_alloc_cell(rt, bytes, EMBI_JS_CELL_STRING);
    s->len_flags = (uint32_t)len | (wide ? EMBI_JS_STR_WIDE_BIT : 0u);
    return s;
}

/* ------------------------------------------------------------------ rope nodes */

static embi_js_string *as_str(embi_js_value v)
{
    return embi_js_as_string(v);
}

/* A rope node that has not been flattened: its children are live. */
static bool is_live_rope(const embi_js_string *s)
{
    return embi_js_str_is_rope(s) && (s->hdr.flags & EMBI_JS_STR_FLATTENED) == 0;
}

/* Flattened ropes stand for their flat string. Other strings stand for themselves. */
static embi_js_string *norm(embi_js_string *s)
{
    if (embi_js_str_is_rope(s) && (s->hdr.flags & EMBI_JS_STR_FLATTENED) != 0) {
        return as_str(((embi_js_rope *)s)->left);
    }
    return s;
}

static uint32_t rope_depth(const embi_js_string *s)
{
    return is_live_rope(s) ? s->hdr.aux : 0u;
}

/* Marks the children of a rope. Flat strings hold no references. */
static void trace_string(embi_js_cell *c, embi_js_tracer *t)
{
    if ((c->flags & EMBI_JS_STR_ROPE) == 0) {
        return;
    }
    embi_js_rope *r = (embi_js_rope *)c;
    embi_js_mark_value(t, r->left);
    embi_js_mark_value(t, r->right);
}

/* The STRING cell type gets its trace function the first time a rope is built, so flat-only
 * runtimes never pay for it. Before that point no STRING cell has children. */
static void ensure_string_ops(embi_js_runtime *rt)
{
    if (rt->heap.ops[EMBI_JS_CELL_STRING].trace == nullptr) {
        embi_js_heap_set_ops(&rt->heap, EMBI_JS_CELL_STRING,
                             (embi_js_type_ops){trace_string, nullptr});
    }
}

/* Walks the leaves of a string in order with an explicit C-heap stack. It never allocates GC
 * cells, so it is safe inside the non-allocating queries. */
typedef struct str_walk {
    embi_js_value *stack;
    size_t len;
    size_t cap;
    const embi_js_string *leaf; /* current flat leaf; nullptr at the end */
} str_walk;

static void walk_push(str_walk *w, embi_js_value v)
{
    if (w->len == w->cap) {
        size_t ncap = w->cap == 0 ? 16u : w->cap * 2u;
        w->stack = embi_realloc(w->stack, w->cap * sizeof *w->stack, ncap * sizeof *w->stack);
        w->cap = ncap;
    }
    w->stack[w->len++] = v;
}

/* Advances to the next non-empty flat leaf. */
static void walk_next(str_walk *w)
{
    w->leaf = nullptr;
    while (w->len != 0) {
        embi_js_string *s = as_str(w->stack[--w->len]);
        while (embi_js_str_is_rope(s)) {
            embi_js_rope *r = (embi_js_rope *)s;
            if ((r->hdr.flags & EMBI_JS_STR_FLATTENED) != 0) {
                s = as_str(r->left);
                break;
            }
            walk_push(w, r->right);
            s = as_str(r->left);
        }
        if (embi_js_str_len(s) != 0) {
            w->leaf = s;
            return;
        }
    }
}

static void walk_start(str_walk *w, const embi_js_string *root)
{
    w->stack = nullptr;
    w->len = 0;
    w->cap = 0;
    w->leaf = nullptr;
    walk_push(w, embi_js_from_cell(root));
    walk_next(w);
}

static void walk_end(str_walk *w)
{
    embi_free(w->stack, w->cap * sizeof *w->stack);
    w->stack = nullptr;
    w->cap = 0;
}

/* Copies a flat leaf into dst at offset `at`, widening when dst is wide. A narrow dst implies a
 * narrow leaf (the representation is canonical). */
static void copy_leaf(embi_js_string *dst, uint32_t at, const embi_js_string *leaf)
{
    uint32_t n = embi_js_str_len(leaf);
    if (embi_js_str_is_wide(dst)) {
        uint16_t *d = wide_mut(dst) + at;
        if (embi_js_str_is_wide(leaf)) {
            memcpy(d, embi_js_str_wide(leaf), (size_t)n * 2u);
        } else {
            const uint8_t *src = embi_js_str_latin1(leaf);
            for (uint32_t i = 0; i < n; i++) {
                d[i] = src[i];
            }
        }
    } else {
        EMBI_ASSERT(!embi_js_str_is_wide(leaf));
        memcpy(latin1_mut(dst) + at, embi_js_str_latin1(leaf), n);
    }
}

/* Concatenates two flat strings into a new flat string. May allocate. */
static embi_js_string *join_flat(embi_js_runtime *rt, const embi_js_string *a,
                                 const embi_js_string *b)
{
    uint32_t la = embi_js_str_len(a);
    uint32_t lb = embi_js_str_len(b);
    bool wide = embi_js_str_is_wide(a) || embi_js_str_is_wide(b);
    embi_js_string *s = alloc_string(rt, (size_t)la + lb, wide);
    copy_leaf(s, 0, a);
    copy_leaf(s, la, b);
    return s;
}

/* Flattens a live rope in place: copies the leaves into a new flat string, then turns the rope
 * into an indirection to it, so its children can die. May allocate; the rope stays alive through
 * the caller. */
static embi_js_string *flatten_rope(embi_js_runtime *rt, embi_js_string *s)
{
    EMBI_ASSERT(is_live_rope(s));
    uint32_t n = embi_js_str_len(s);
    bool wide = embi_js_str_is_wide(s);
    embi_js_string *flat = alloc_string(rt, n, wide);

    str_walk w;
    walk_start(&w, s);
    uint32_t pos = 0;
    while (w.leaf != nullptr) {
        copy_leaf(flat, pos, w.leaf);
        pos += embi_js_str_len(w.leaf);
        walk_next(&w);
    }
    walk_end(&w);
    EMBI_ASSERT(pos == n);

    embi_js_rope *r = (embi_js_rope *)s;
    /* The rope may be old while the flat string is young (generational mode): remember it. The
     * new value is a plain store, and EMBI_JS_EMPTY is not a cell, so right needs no barrier. */
    r->left = embi_js_from_string(flat);
    EMBI_JS_WB(r, r->left);
    r->right = EMBI_JS_EMPTY;
    r->hdr.aux = 0;
    r->hdr.flags |= EMBI_JS_STR_FLATTENED;
    return flat;
}

/* The flat string for s, flattening a live rope. */
static embi_js_string *flat_of(embi_js_runtime *rt, embi_js_string *s)
{
    s = norm(s);
    return is_live_rope(s) ? flatten_rope(rt, s) : s;
}

/* Creates a rope node. Its length and wide bit come from the children. */
static embi_js_value make_rope(embi_js_runtime *rt, embi_js_value left, embi_js_value right,
                               uint32_t depth)
{
    ensure_string_ops(rt);
    const embi_js_string *l = as_str(left);
    const embi_js_string *r = as_str(right);
    uint64_t total = (uint64_t)embi_js_str_len(l) + embi_js_str_len(r);
    if (total > EMBI_JS_STR_MAX_LEN) {
        embi_log_error("string length %llu exceeds the engine limit",
                       (unsigned long long)total);
        abort();
    }
    bool wide = embi_js_str_is_wide(l) || embi_js_str_is_wide(r);
    embi_js_rope *node = (embi_js_rope *)embi_js_alloc_cell(rt, sizeof *node, EMBI_JS_CELL_STRING);
    node->len_flags = (uint32_t)total | (wide ? EMBI_JS_STR_WIDE_BIT : 0u);
    node->hdr.flags = EMBI_JS_STR_ROPE;
    node->hdr.aux = depth;
    node->left = left;
    node->right = right;
    return embi_js_from_cell(node);
}

/* ------------------------------------------------------------------ construction */

embi_js_string *embi_js_str_new_latin1(embi_js_runtime *rt, const uint8_t *p, size_t len)
{
    embi_js_string *s = alloc_string(rt, len, false);
    if (len != 0) {
        memcpy(latin1_mut(s), p, len);
    }
    return s;
}

embi_js_string *embi_js_str_new_utf16(embi_js_runtime *rt, const uint16_t *p, size_t len)
{
    bool wide = false;
    for (size_t i = 0; i < len; i++) {
        if (p[i] > 0xFFu) {
            wide = true;
            break;
        }
    }
    embi_js_string *s = alloc_string(rt, len, wide);
    if (wide) {
        if (len != 0) {
            memcpy(wide_mut(s), p, len * 2u);
        }
    } else {
        uint8_t *d = latin1_mut(s);
        for (size_t i = 0; i < len; i++) {
            d[i] = (uint8_t)p[i];
        }
    }
    return s;
}

embi_js_string *embi_js_str_new_utf8(embi_js_runtime *rt, const char *p, size_t len)
{
    /* Checked on the full size_t before anything narrows it. */
    if (len > EMBI_JS_STR_MAX_LEN) {
        embi_log_error("string length %llu exceeds the engine limit",
                       (unsigned long long)len);
        abort();
    }
    bool ascii = true;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)p[i] >= 0x80u) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        return embi_js_str_new_latin1(rt, (const uint8_t *)p, len);
    }

    /* Every input byte yields at most one code unit (a four-byte sequence yields two units),
     * so len units is enough. */
    uint16_t *units = embi_alloc(len * 2u + 2u);
    size_t n = 0;
    const char *cur = p;
    const char *end = p + len;
    while (cur < end) {
        uint32_t cp = embi_utf8_decode(&cur, end);
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            units[n++] = (uint16_t)(0xD800u + (cp >> 10));
            units[n++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
        } else {
            units[n++] = (uint16_t)cp;
        }
    }
    embi_js_string *s = embi_js_str_new_utf16(rt, units, n);
    embi_free(units, len * 2u + 2u);
    return s;
}

/* Copies [start, start + count) of src into a new string, narrowing when the slice allows. src
 * must be flat. */
static embi_js_string *slice_string(embi_js_runtime *rt, const embi_js_string *src,
                                    uint32_t start, uint32_t count)
{
    bool wide = false;
    if (embi_js_str_is_wide(src)) {
        for (uint32_t i = 0; i < count; i++) {
            if (embi_js_str_at(src, start + i) > 0xFFu) {
                wide = true;
                break;
            }
        }
    }
    embi_js_string *d = alloc_string(rt, count, wide);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t u = embi_js_str_at(src, start + i);
        if (wide) {
            wide_mut(d)[i] = u;
        } else {
            latin1_mut(d)[i] = (uint8_t)u;
        }
    }
    return d;
}

embi_js_string *embi_js_str_flat(embi_js_runtime *rt, embi_js_value v)
{
    return flat_of(rt, as_str(v));
}

embi_js_string *embi_js_str_concat(embi_js_runtime *rt, const embi_js_string *a,
                                   const embi_js_string *b)
{
    embi_js_string *fa = flat_of(rt, (embi_js_string *)a);
    embi_js_string *fb = flat_of(rt, (embi_js_string *)b);
    return join_flat(rt, fa, fb);
}

embi_js_value embi_js_str_concat_v(embi_js_runtime *rt, embi_js_value av, embi_js_value bv)
{
    embi_js_string *a = norm(as_str(av));
    embi_js_string *b = norm(as_str(bv));
    uint32_t la = embi_js_str_len(a);
    uint32_t lb = embi_js_str_len(b);
    if (la == 0) {
        return embi_js_from_string(b);
    }
    if (lb == 0) {
        return embi_js_from_string(a);
    }
    uint64_t total = (uint64_t)la + lb;
    if (total > EMBI_JS_STR_MAX_LEN) {
        embi_log_error("string length %llu exceeds the engine limit",
                       (unsigned long long)total);
        abort();
    }

    /* A deep rope is flattened here, which bounds the depth (and the walkers' stacks). */
    if (rope_depth(a) > EMBI_JS_ROPE_DEPTH_MAX) {
        a = flatten_rope(rt, a);
    }
    if (rope_depth(b) > EMBI_JS_ROPE_DEPTH_MAX) {
        b = flatten_rope(rt, b);
    }

    if (total < rt->rope_min) {
        /* Sequenced: the first flat result must stay reachable while the second one allocates. */
        embi_js_string *fa = flat_of(rt, a);
        embi_js_string *fb = flat_of(rt, b);
        return embi_js_from_string(join_flat(rt, fa, fb));
    }

    /* Appending a short flat part to a rope whose right leaf is short: merge the leaves, so
     * character-at-a-time appends keep one short leaf instead of one node per character. */
    if (is_live_rope(a) && lb < EMBI_JS_ROPE_LEAF_MAX) {
        embi_js_rope *ra = (embi_js_rope *)a;
        const embi_js_string *right = as_str(ra->right);
        if (!embi_js_str_is_rope(right) && embi_js_str_len(right) < EMBI_JS_ROPE_LEAF_MAX) {
            uint32_t depth = ra->hdr.aux;
            embi_js_string *bf = flat_of(rt, b);
            embi_js_string *leaf = join_flat(rt, right, bf);
            return make_rope(rt, ra->left, embi_js_from_string(leaf), depth);
        }
    }

    uint32_t da = rope_depth(a);
    uint32_t db = rope_depth(b);
    uint32_t depth = (da > db ? da : db) + 1u;
    return make_rope(rt, embi_js_from_string(a), embi_js_from_string(b), depth);
}

embi_js_string *embi_js_str_substring(embi_js_runtime *rt, const embi_js_string *s,
                                      uint32_t start, uint32_t end)
{
    embi_js_string *f = flat_of(rt, (embi_js_string *)s);
    uint32_t n = embi_js_str_len(f);
    if (end > n) {
        end = n;
    }
    if (start > end) {
        start = end;
    }
    if (start == 0 && end == n) {
        return f; /* strings are immutable */
    }
    return slice_string(rt, f, start, end - start);
}

/* ------------------------------------------------------------------ queries */

/* Non-allocating views for the helpers that take no runtime. A rope is copied into a C-heap
 * flat buffer (a header with the length and width, then the units) that the caller frees with
 * free_view. A flat string is returned as is. */
static const embi_js_string *view_flat(const embi_js_string *s, embi_js_string **owned)
{
    *owned = nullptr;
    if (!embi_js_str_is_rope(s)) {
        return s;
    }
    uint32_t n = embi_js_str_len(s);
    bool wide = embi_js_str_is_wide(s);
    size_t bytes = (size_t)n * (wide ? 2u : 1u);
    embi_js_string *buf = embi_alloc(sizeof(embi_js_string) + bytes);
    buf->hdr.type = EMBI_JS_CELL_STRING;
    buf->hdr.mark = 0;
    buf->hdr.flags = 0;
    buf->hdr.aux = 0;
    buf->len_flags = (uint32_t)n | (wide ? EMBI_JS_STR_WIDE_BIT : 0u);
    buf->hash = 0;

    str_walk w;
    walk_start(&w, s);
    uint32_t pos = 0;
    while (w.leaf != nullptr) {
        copy_leaf(buf, pos, w.leaf);
        pos += embi_js_str_len(w.leaf);
        walk_next(&w);
    }
    walk_end(&w);
    *owned = buf;
    return buf;
}

static void free_view(embi_js_string *owned)
{
    if (owned == nullptr) {
        return;
    }
    size_t n = embi_js_str_len(owned);
    size_t bytes = sizeof(embi_js_string) + n * (embi_js_str_is_wide(owned) ? 2u : 1u);
    embi_free(owned, bytes);
}

/* Compares the common prefix of two strings, unit by unit in code-unit order. Returns the sign of
 * the first difference, or 0 when the shorter one is a prefix of the other. */
static int run_cmp(const embi_js_string *x, uint32_t px, const embi_js_string *y, uint32_t py,
                   uint32_t k)
{
    if (!embi_js_str_is_wide(x) && !embi_js_str_is_wide(y)) {
        int c = memcmp(embi_js_str_latin1(x) + px, embi_js_str_latin1(y) + py, k);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    for (uint32_t i = 0; i < k; i++) {
        uint16_t ux = embi_js_str_at(x, px + i);
        uint16_t uy = embi_js_str_at(y, py + i);
        if (ux != uy) {
            return ux < uy ? -1 : 1;
        }
    }
    return 0;
}

/* Compares the common prefix of two strings of any shape (walks both leaf sequences). */
static int walk_cmp(const embi_js_string *a, const embi_js_string *b)
{
    str_walk wa;
    str_walk wb;
    walk_start(&wa, a);
    walk_start(&wb, b);
    uint32_t pa = 0;
    uint32_t pb = 0;
    int result = 0;
    while (wa.leaf != nullptr && wb.leaf != nullptr) {
        uint32_t ra = embi_js_str_len(wa.leaf) - pa;
        uint32_t rb = embi_js_str_len(wb.leaf) - pb;
        uint32_t k = ra < rb ? ra : rb;
        result = run_cmp(wa.leaf, pa, wb.leaf, pb, k);
        if (result != 0) {
            break;
        }
        pa += k;
        pb += k;
        if (pa == embi_js_str_len(wa.leaf)) {
            walk_next(&wa);
            pa = 0;
        }
        if (pb == embi_js_str_len(wb.leaf)) {
            walk_next(&wb);
            pb = 0;
        }
    }
    walk_end(&wa);
    walk_end(&wb);
    return result;
}

bool embi_js_str_equal(const embi_js_string *a, const embi_js_string *b)
{
    uint32_t n = embi_js_str_len(a);
    if (n != embi_js_str_len(b)) {
        return false;
    }
    /* Representation is canonical, so differing widths mean differing contents. */
    if (embi_js_str_is_wide(a) != embi_js_str_is_wide(b)) {
        return false;
    }
    if (a->hash != 0 && b->hash != 0 && a->hash != b->hash) {
        return false;
    }
    if (embi_js_str_is_rope(a) || embi_js_str_is_rope(b)) {
        return walk_cmp(a, b) == 0;
    }
    size_t w = embi_js_str_is_wide(a) ? 2u : 1u;
    if (w == 1u) {
        return memcmp(embi_js_str_latin1(a), embi_js_str_latin1(b), n) == 0;
    }
    return memcmp(embi_js_str_wide(a), embi_js_str_wide(b), (size_t)n * 2u) == 0;
}

int embi_js_str_compare(const embi_js_string *a, const embi_js_string *b)
{
    uint32_t la = embi_js_str_len(a);
    uint32_t lb = embi_js_str_len(b);
    uint32_t n = la < lb ? la : lb;
    int c;
    if (embi_js_str_is_rope(a) || embi_js_str_is_rope(b)) {
        c = walk_cmp(a, b);
    } else {
        c = run_cmp(a, 0, b, 0, n);
    }
    if (c != 0) {
        return c;
    }
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

/* Hash over code units, so a rope and the flat string with the same content agree. FNV-1a over
 * units, then the murmur3 finalizer (the atom table uses the low bits). */
#define HASH_SEED EMBI_JS_STR_HASH_SEED
#define HASH_PRIME EMBI_JS_STR_HASH_PRIME

static uint32_t hash_leaf(uint32_t h, const embi_js_string *leaf)
{
    uint32_t n = embi_js_str_len(leaf);
    if (embi_js_str_is_wide(leaf)) {
        const uint16_t *u = embi_js_str_wide(leaf);
        for (uint32_t i = 0; i < n; i++) {
            h = (h ^ u[i]) * HASH_PRIME;
        }
    } else {
        const uint8_t *u = embi_js_str_latin1(leaf);
        for (uint32_t i = 0; i < n; i++) {
            h = (h ^ u[i]) * HASH_PRIME;
        }
    }
    return h;
}

uint32_t embi_js_str_hash(const embi_js_string *s)
{
    if (s->hash != 0) {
        return s->hash;
    }
    uint32_t h = HASH_SEED;
    if (!embi_js_str_is_rope(s)) {
        h = hash_leaf(h, s);
    } else {
        str_walk w;
        walk_start(&w, s);
        while (w.leaf != nullptr) {
            h = hash_leaf(h, w.leaf);
            walk_next(&w);
        }
        walk_end(&w);
    }
    h = embi_js_str_hash_finish(h);
    ((embi_js_string *)s)->hash = h;
    return h;
}

static void append_utf8_flat(const embi_js_string *s, embi_sb *sb)
{
    uint32_t n = embi_js_str_len(s);
    if (!embi_js_str_is_wide(s)) {
        const uint8_t *src = embi_js_str_latin1(s);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = src[i];
            if (b < 0x80u) {
                embi_sb_append_char(sb, (char)b);
            } else {
                char tmp[4];
                size_t k = embi_utf8_encode(b, tmp);
                embi_sb_append(sb, (embi_str){tmp, (uint32_t)k});
            }
        }
        return;
    }
    if (n == 0) {
        return;
    }
    /* A unit encodes to at most three bytes, and a surrogate pair (two units) to four. */
    size_t cap = 3u * (size_t)n;
    char *buf = embi_alloc(cap);
    size_t k = embi_utf16_to_utf8(embi_js_str_wide(s), n, buf, cap);
    embi_sb_append(sb, (embi_str){buf, (uint32_t)k});
    embi_free(buf, cap);
}

void embi_js_str_append_utf8(const embi_js_string *s, embi_sb *sb)
{
    embi_js_string *owned;
    const embi_js_string *f = view_flat(s, &owned);
    append_utf8_flat(f, sb);
    free_view(owned);
}

static bool is_array_index_flat(const embi_js_string *s, uint32_t *out)
{
    uint32_t n = embi_js_str_len(s);
    if (n == 0 || n > 10u) {
        return false;
    }
    uint16_t c0 = embi_js_str_at(s, 0);
    if (c0 < '0' || c0 > '9' || (c0 == '0' && n > 1u)) {
        return false;
    }
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = embi_js_str_at(s, i);
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10u + (uint64_t)(c - '0');
    }
    if (v > EMBI_JS_MAX_ARRAY_INDEX) {
        return false;
    }
    if (out != nullptr) {
        *out = (uint32_t)v;
    }
    return true;
}

bool embi_js_str_is_array_index(const embi_js_string *s, uint32_t *out)
{
    if (embi_js_str_len(s) == 0 || embi_js_str_len(s) > 10u) {
        return false;
    }
    embi_js_string *owned;
    const embi_js_string *f = view_flat(s, &owned);
    bool r = is_array_index_flat(f, out);
    free_view(owned);
    return r;
}

embi_js_key embi_js_key_from_str(embi_js_runtime *rt, const embi_js_string *s)
{
    embi_js_string *f = flat_of(rt, (embi_js_string *)s);
    uint32_t idx = 0;
    if (embi_js_str_is_array_index(f, &idx)) {
        return embi_js_key_from_index(idx);
    }
    return embi_js_from_string(embi_js_atom_intern(rt, f));
}

/* ------------------------------------------------------------------ number to string */

static size_t fmt_u32(uint32_t v, char *out)
{
    char tmp[10];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    return n;
}

static bool is_negative_zero(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    return bits == 0x8000000000000000ull;
}

/* Number::toString. Shortest round-trip digits and the ECMA-262 layout come from src/num, which
 * is locale-independent and exact. */
embi_js_string *embi_js_number_to_str(embi_js_runtime *rt, double d)
{
    char buf[EMBI_NUM_STR_MAX];
    size_t n;
    if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d &&
        !is_negative_zero(d)) {
        int32_t i = (int32_t)d;
        size_t o = 0;
        uint32_t mag;
        if (i < 0) {
            buf[o++] = '-';
            mag = (uint32_t)0 - (uint32_t)i;
        } else {
            mag = (uint32_t)i;
        }
        o += fmt_u32(mag, buf + o);
        n = o;
    } else {
        n = embi_num_to_str(d, buf);
    }
    return embi_js_str_new_latin1(rt, (const uint8_t *)buf, n);
}

/* ------------------------------------------------------------------ string to number */

static bool is_js_space(uint16_t c)
{
    switch (c) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return true;
    default:
        return c >= 0x2000u && c <= 0x200Au;
    }
}

static uint32_t radix_digit(uint16_t c)
{
    if (c >= '0' && c <= '9') {
        return (uint32_t)(c - '0');
    }
    if (c >= 'a' && c <= 'z') {
        return (uint32_t)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'Z') {
        return (uint32_t)(c - 'A' + 10);
    }
    return 99u;
}

/* Copies the validated ASCII units [start, end) into a buffer and parses them with src/num.
 * `radix` 10 means a StrDecimalLiteral (sign allowed); 2, 8 or 16 means digits only. */
static double parse_ascii_units(const embi_js_string *s, uint32_t start, uint32_t end,
                                unsigned radix)
{
    size_t n = end - start;
    char *buf = embi_alloc(n + 1u);
    for (size_t k = 0; k < n; k++) {
        buf[k] = (char)embi_js_str_at(s, start + (uint32_t)k);
    }
    buf[n] = '\0';
    size_t used = 0;
    double v = radix == 10u ? embi_num_parse_decimal(buf, n, &used)
                            : embi_num_parse_radix(buf, n, radix, &used);
    EMBI_ASSERT(used == n);
    (void)used;
    embi_free(buf, n + 1u);
    return v;
}

static double str_to_number_flat(const embi_js_string *s)
{
    uint32_t i = 0;
    uint32_t end = embi_js_str_len(s);
    while (i < end && is_js_space(embi_js_str_at(s, i))) {
        i++;
    }
    while (end > i && is_js_space(embi_js_str_at(s, end - 1u))) {
        end--;
    }
    if (i == end) {
        return 0.0;
    }

    if (end - i > 2u && embi_js_str_at(s, i) == '0') {
        uint16_t x = embi_js_str_at(s, i + 1u);
        unsigned bits = 0;
        if (x == 'x' || x == 'X') {
            bits = 4;
        } else if (x == 'o' || x == 'O') {
            bits = 3;
        } else if (x == 'b' || x == 'B') {
            bits = 1;
        }
        if (bits != 0) {
            for (uint32_t j = i + 2u; j < end; j++) {
                if (radix_digit(embi_js_str_at(s, j)) >= (1u << bits)) {
                    return __builtin_nan("");
                }
            }
            return parse_ascii_units(s, i + 2u, end, 1u << bits);
        }
    }

    /* StrDecimalLiteral, with an optional sign and the literal "Infinity". */
    uint32_t p = i;
    bool neg = false;
    if (embi_js_str_at(s, p) == '+' || embi_js_str_at(s, p) == '-') {
        neg = embi_js_str_at(s, p) == '-';
        p++;
    }
    static const char inf_text[] = "Infinity";
    if (end - p == 8u) {
        bool match = true;
        for (uint32_t k = 0; k < 8u; k++) {
            if (embi_js_str_at(s, p + k) != (uint16_t)inf_text[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            return neg ? -__builtin_inf() : __builtin_inf();
        }
    }

    uint32_t q = p;
    uint32_t digits = 0;
    while (q < end && embi_js_str_at(s, q) >= '0' && embi_js_str_at(s, q) <= '9') {
        q++;
        digits++;
    }
    if (q < end && embi_js_str_at(s, q) == '.') {
        q++;
        while (q < end && embi_js_str_at(s, q) >= '0' && embi_js_str_at(s, q) <= '9') {
            q++;
            digits++;
        }
    }
    if (digits == 0) {
        return __builtin_nan("");
    }
    if (q < end && (embi_js_str_at(s, q) == 'e' || embi_js_str_at(s, q) == 'E')) {
        q++;
        if (q < end && (embi_js_str_at(s, q) == '+' || embi_js_str_at(s, q) == '-')) {
            q++;
        }
        uint32_t exp_digits = 0;
        while (q < end && embi_js_str_at(s, q) >= '0' && embi_js_str_at(s, q) <= '9') {
            q++;
            exp_digits++;
        }
        if (exp_digits == 0) {
            return __builtin_nan("");
        }
    }
    if (q != end) {
        return __builtin_nan("");
    }

    /* The whole literal (sign included) is valid StrDecimalLiteral syntax, so src/num parses all of
     * it and reports the full length. */
    return parse_ascii_units(s, i, end, 10u);
}

double embi_js_str_to_number(const embi_js_string *s)
{
    embi_js_string *owned;
    const embi_js_string *f = view_flat(s, &owned);
    double v = str_to_number_flat(f);
    free_view(owned);
    return v;
}

void embi_js_set_rope_min(embi_js_runtime *rt, uint32_t n)
{
    rt->rope_min = n != 0 ? n : 256;
}

void embi_js_str_append_wtf8(const embi_js_string *s, embi_sb *sb)
{
    embi_js_string *owned;
    const embi_js_string *f = view_flat(s, &owned);
    if (!embi_js_str_is_wide(f)) {
        append_utf8_flat(f, sb);
        free_view(owned);
        return;
    }
    uint32_t n = embi_js_str_len(f);
    const uint16_t *w = embi_js_str_wide(f);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = w[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1u < n && w[i + 1u] >= 0xDC00u &&
            w[i + 1u] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (w[i + 1u] - 0xDC00u);
            i++;
        }
        char tmp[4];
        size_t k;
        if (cp < 0x80u) {
            tmp[0] = (char)cp;
            k = 1;
        } else if (cp < 0x800u) {
            tmp[0] = (char)(0xC0u | (cp >> 6));
            tmp[1] = (char)(0x80u | (cp & 0x3Fu));
            k = 2;
        } else if (cp < 0x10000u) {
            /* Lone surrogates take this branch too (WTF-8). */
            tmp[0] = (char)(0xE0u | (cp >> 12));
            tmp[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            tmp[2] = (char)(0x80u | (cp & 0x3Fu));
            k = 3;
        } else {
            tmp[0] = (char)(0xF0u | (cp >> 18));
            tmp[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            tmp[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            tmp[3] = (char)(0x80u | (cp & 0x3Fu));
            k = 4;
        }
        embi_sb_append(sb, (embi_str){tmp, (uint32_t)k});
    }
    free_view(owned);
}
