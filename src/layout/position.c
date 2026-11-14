/*
 * Positioned layout (CSS Position 3 sections 2 to 5, CSS 2.1 sections 10.3.7 and 10.6.4), W3d
 * section 6.
 *
 * Out-of-flow children are skipped by block flow (block.c) and by inline layout (block.c, through
 * embi_lay_abspos_ifc), which only record their static position here. Nothing is laid out at that
 * point, because the containing blocks are not placed yet. embi_lay_position_finish runs in this
 * order:
 *   1. The flow tree gets its final relative offsets (CSS Position 3 section 3.2) and its stacking
 *      flags. A relative box moves its own fragment, and the fragments inside it, so every static
 *      position and every inline containing block read below is already in its shifted place.
 *   2. The records are placed in the order they were made, as a queue: a box placed earlier can
 *      contain a box recorded later (the records that a placed box's layout creates are appended
 * and placed after it). A record is placed by these steps:
 *      - the containing block is the nearest ancestor that is positioned (for absolute), or that
 *        establishes a containing block for all descendants (transform, filter, contain, ...), or
 *        the initial containing block (the viewport for fixed). An inline containing block is the
 *        padding box of its first and last fragments.
 *      - the static position, recorded relative to the static container's border box, is moved into
 *        the containing block's frame by summing the fragment positions in between (an inline
 *        container's relative offsets count too).
 *      - the box is laid out with an available width and percentage bases from the containing
 * block, then its insets are solved (CSS 2.1 10.3.7 and 10.6.4, with auto margins and the over-
 *        constrained rules in the direction of the containing block), the same relative and
 *        stacking pass runs over its subtree, and it is appended as the last child of the
 *        containing block's fragment, so it paints above the earlier siblings in tree order.
 *      A later record for the same box replaces the earlier ones, which are the layouts of a
 * container that was run again (overflow: auto, floats, clearance), so the final layout's records
 * win.
 *   3. The sticky data is stored (embi_lay_sticky_offset applies it on scroll).
 *
 * Flex and grid containers hook in through embi_lay_abspos_at: they pass the static position of
 * their children, and for grid, the grid area as the containing block.
 */
#include "layout_int.h"

#include "css/props.h"
#include "layout/xform.h"

#include <math.h>
#include <stdalign.h>
#include <string.h>

/* One out-of-flow box waiting for its placement. */
typedef struct embi_pos_rec {
    embi_box *box;  /* the out-of-flow box */
    embi_box *sbox; /* the static container: its border box is the origin of sx and sy */
    float sx, sy;   /* the static position of the margin edge: the inline start (the right edge in
                     * rtl) and the block start */
    bool has_cb;    /* the containing block is cb (grid areas), not found from the box tree */
    float cb[4];    /* x0, y0, x1, y1 of cb, relative to sbox's border box */
    /* A child of a flex container (W3e): its static position is the sole-item position in the
     * container's content box fcont (x0, y0, x1, y1 relative to sbox's border box), at the fraction
     * ffrac of the free space on each axis (0 start, 0.5 center, 1 end), from the flipped start edge
     * when fflip. Used when both insets of an axis are auto. */
    bool flex;
    float fcont[4];
    float ffrac[2];
    bool fflip[2];
    /* The static position is the box's end edge, not its start edge (the placeholder's bidi direction is
     * the opposite of the static container's, embi_lay_abspos_flip). */
    bool sflip;
} embi_pos_rec;

/* The first and last fragments, in tree order, of an inline box (an inline containing block). */
typedef struct embi_icache_ent {
    const embi_box *b;
    embi_frag *first;
    embi_frag *last;
} embi_icache_ent;

struct embi_pos {
    EMBI_VEC(embi_pos_rec) recs;
    /* box -> index in recs of its last record. Kept current as records are pushed, also while the
     * records are being placed (a placed box's layout pushes more). Allocation failure aborts in
     * the base allocator (embi_oom), so a put never fails for memory: its false result only means
     * that the key was already there and its value was replaced. */
    embi_hashmap last;
    EMBI_VEC(embi_icache_ent) ic;
    embi_hashmap icmap; /* inline box -> index in ic */
    bool ic_ready;      /* ic covers the fragments placed so far (built on first use) */
};

/* The sticky data of a fragment (fragment.h). Rectangles are in the frame of the fragment's parent
 * at zero scroll offset, so a scroll offset (sx, sy) of the scroll container moves the view by
 * (sx, sy). */
struct embi_sticky {
    float inset[4];          /* top, right, bottom, left: NaN when auto */
    float view[4];           /* the scrollport: x0, y0, x1, y1 */
    float cont[4];           /* the containing block's content box: x0, y0, x1, y1 */
    const embi_frag *scroll; /* the scroll container, or the initial containing block */
    float ox, oy;            /* the line fragments' position to add to the fragment's x and y */
    float mg[4];             /* the specified margins (auto: 0): top, right, bottom, left */
};

/* ------------------------------------------------------------------ style helpers */

static float nz(float v) { return isnan(v) ? 0.0f : v; }

static bool is_positioned(uint16_t pos) {
    return pos == EMBI_KW_relative || pos == EMBI_KW_absolute || pos == EMBI_KW_fixed ||
           pos == EMBI_KW_sticky;
}

/* The boxes that have the style of an element (not text, markers and line breaks): only these can
 * be positioned or establish containing blocks. */
static bool has_style(const embi_box *b) {
    if (b->style == nullptr) {
        return false;
    }
    switch (b->kind) {
    case EMBI_BK_BLOCK:
    case EMBI_BK_INLINE:
    case EMBI_BK_REPLACED:
    case EMBI_BK_FLEX:
    case EMBI_BK_GRID:
    case EMBI_BK_TABLE:
        return true;
    default:
        return false;
    }
}

static uint16_t kw_of(const embi_lctx *c, const embi_box *b, embi_prop_id p) {
    const embi_value *v = embi_lay_prop(c, b, p);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

/* The position keyword of an alignment property (justify-self, align-self): a keyword, or the
 * position of a `safe`/`unsafe` pair (a list). 0 when there is none. */
static uint16_t align_kw(const embi_lctx *c, const embi_box *b, embi_prop_id p) {
    const embi_value *v = embi_lay_prop(c, b, p);
    if (v == nullptr) {
        return 0;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        return v->kw;
    }
    if (v->kind == EMBI_V_LIST && v->list.n == 2 && v->list.items[1].kind == EMBI_V_KEYWORD) {
        if (v->list.items[0].kind == EMBI_V_KEYWORD && v->list.items[0].kw == EMBI_KW_last &&
            v->list.items[1].kw == EMBI_KW_baseline) {
            return EMBI_KW_end; /* the fallback alignment of last baseline is end (CSS Align 3 section 9.3) */
        }
        return v->list.items[1].kw;
    }
    return 0;
}

/* The offset of an alignment inside the free space slack: center moves by half, end by all of it,
 * and start (the default) not at all. The direction is the caller's: it picks which edge is the
 * end (CSS Position 3 section 5). */
static float align_offset(uint16_t kw, float slack) {
    if (kw == EMBI_KW_center) {
        return slack / 2;
    }
    if (kw == EMBI_KW_end || kw == EMBI_KW_self_end || kw == EMBI_KW_flex_end) {
        return slack;
    }
    return 0;
}

static bool is_end_align(uint16_t kw) {
    return kw == EMBI_KW_end || kw == EMBI_KW_self_end || kw == EMBI_KW_flex_end;
}

static uint16_t pos_of(const embi_lctx *c, const embi_box *b) {
    return has_style(b) ? kw_of(c, b, EMBI_PROP_position) : 0;
}

static bool is_none(const embi_value *v) {
    return v == nullptr || v->kind == EMBI_V_NONE ||
           (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none);
}

static bool ident_is(embi_atom a, const char *lit) {
    embi_str s = embi_atom_str(a);
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

/* True when a will-change list (or single ident) names the given property. */
static bool wc_has(const embi_value *v, const char *lit) {
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_IDENT) {
        return ident_is(v->ident, lit);
    }
    if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            if (wc_has(&v->list.items[i], lit)) {
                return true;
            }
        }
    }
    return false;
}

/* contain: layout, paint, strict or content (a keyword or a list of keywords). */
static bool kw_contains(uint16_t k) {
    return k == EMBI_KW_layout || k == EMBI_KW_paint || k == EMBI_KW_strict || k == EMBI_KW_content;
}

static bool contain_creates_prop(const embi_style *s) {
    const embi_value *v = embi_style_value(s, EMBI_PROP_contain);
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        return kw_contains(v->kw);
    }
    if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            const embi_value *k = &v->list.items[i];
            if (k->kind == EMBI_V_KEYWORD && kw_contains(k->kw)) {
                return true;
            }
        }
    }
    return false;
}

/* Layout containment from any source: contain: layout|paint|strict|content, container-type: size or
 * inline-size, content-visibility: hidden or auto. One predicate for the stacking context and the
 * containing block of fixed and absolute descendants, so the two cannot drift. */
static bool contain_creates(const embi_style *s) {
    if (contain_creates_prop(s)) {
        return true;
    }
    const embi_value *ct = embi_style_value(s, EMBI_PROP_container_type);
    if (ct != nullptr && ct->kind == EMBI_V_KEYWORD &&
        (ct->kw == EMBI_KW_size || ct->kw == EMBI_KW_inline_size)) {
        return true;
    }
    const embi_value *cv = embi_style_value(s, EMBI_PROP_content_visibility);
    return cv != nullptr && cv->kind == EMBI_V_KEYWORD &&
           (cv->kw == EMBI_KW_hidden || cv->kw == EMBI_KW_auto);
}

/* transform-style: preserve-3d (R3c): it makes a stacking context and, as in browsers, a containing block for all
 * descendants. */
static bool preserves_3d(const embi_style *s) {
    const embi_value *v = embi_style_value(s, EMBI_PROP_transform_style);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_preserve_3d;
}

/* backface-visibility: hidden: as in browsers, such a box is a stacking context so that it can be a plane of a 3D
 * rendering context whose parent is turned away. */
static bool backface_hidden(const embi_style *s) {
    const embi_value *v = embi_style_value(s, EMBI_PROP_backface_visibility);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_hidden;
}

/* A box that establishes a containing block for all its descendants (CSS Position 3 section 2):
 * transform, perspective, filter, backdrop-filter, contain: layout|paint, or will-change naming one
 * of them. Boxes that are not transformable (inline) never do. */
static bool cb_all_style(const embi_style *s) {
    if (!is_none(embi_style_value(s, EMBI_PROP_transform)) ||
        !is_none(embi_style_value(s, EMBI_PROP_perspective)) || preserves_3d(s) ||
        !is_none(embi_style_value(s, EMBI_PROP_filter)) ||
        !is_none(embi_style_value(s, EMBI_PROP_backdrop_filter)) || contain_creates(s)) {
        return true;
    }
    const embi_value *wc = embi_style_value(s, EMBI_PROP_will_change);
    return wc_has(wc, "transform") || wc_has(wc, "perspective") || wc_has(wc, "filter");
}

/* The style-only facts the box build and the stacking pass read for every box, computed once per style
 * (W3q: a rebuild asked for about 25 properties per box, 144k style reads per layout of the bench page).
 * The cache is direct-mapped by style pointer and dropped with the box tree. */
static uint16_t kwp(const embi_style *s, embi_prop_id p) {
    const embi_value *v = embi_style_value(s, p);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : (uint16_t)0;
}

static void ssum_compute(embi_lay_ssum *o, const embi_style *s) {
    memset(o, 0, sizeof *o);
    o->key = s;
    const embi_value *d = embi_style_value(s, EMBI_PROP_display);
    o->disp = d == nullptr || d->kind != EMBI_V_KEYWORD ? (uint16_t)EMBI_KW_block : d->kw;
    o->pos = kwp(s, EMBI_PROP_position);
    o->flt = kwp(s, EMBI_PROP_float);
    o->ox = kwp(s, EMBI_PROP_overflow_x);
    o->oy = kwp(s, EMBI_PROP_overflow_y);
    /* make_box: layout and paint containment (contain, container-type) */
    {
        bool lay = false, paint = false;
        const embi_value *cv = embi_style_value(s, EMBI_PROP_contain);
        if (cv != nullptr && cv->kind == EMBI_V_KEYWORD) {
            lay = cv->kw == EMBI_KW_layout || cv->kw == EMBI_KW_strict || cv->kw == EMBI_KW_content;
            paint = cv->kw == EMBI_KW_paint || cv->kw == EMBI_KW_strict || cv->kw == EMBI_KW_content;
        } else if (cv != nullptr && cv->kind == EMBI_V_LIST) {
            for (uint32_t i = 0; i < cv->list.n; i++) {
                const embi_value *k = &cv->list.items[i];
                if (k->kind != EMBI_V_KEYWORD) {
                    continue;
                }
                lay = lay || k->kw == EMBI_KW_layout || k->kw == EMBI_KW_strict || k->kw == EMBI_KW_content;
                paint = paint || k->kw == EMBI_KW_paint || k->kw == EMBI_KW_strict || k->kw == EMBI_KW_content;
            }
        }
        uint16_t ctype = kwp(s, EMBI_PROP_container_type);
        if (ctype == EMBI_KW_size || ctype == EMBI_KW_inline_size) {
            lay = true;
        }
        o->contain_lay = lay;
        o->contain_paint = paint;
    }
    /* set_flags: z-index, and the properties that make a stacking context */
    const embi_value *zv = embi_style_value(s, EMBI_PROP_z_index);
    o->zauto = !(zv != nullptr && (zv->kind == EMBI_V_INTEGER || zv->kind == EMBI_V_NUMBER));
    if (!o->zauto) {
        double z = zv->num;
        o->zi = z >= 2147483647.0 ? INT32_MAX : z <= -2147483648.0 ? INT32_MIN : (int32_t)z;
    }
    bool st = false;
    const embi_value *op = embi_style_value(s, EMBI_PROP_opacity);
    st = st || (op != nullptr && op->kind == EMBI_V_NUMBER && op->num < 1.0);
    st = st || !is_none(embi_style_value(s, EMBI_PROP_filter)) ||
         !is_none(embi_style_value(s, EMBI_PROP_backdrop_filter));
    const embi_value *iso = embi_style_value(s, EMBI_PROP_isolation);
    st = st || (iso != nullptr && iso->kind == EMBI_V_KEYWORD && iso->kw == EMBI_KW_isolate);
    const embi_value *mb = embi_style_value(s, EMBI_PROP_mix_blend_mode);
    st = st || (mb != nullptr && mb->kind == EMBI_V_KEYWORD && mb->kw != EMBI_KW_normal);
    st = st || !is_none(embi_style_value(s, EMBI_PROP_clip_path)) ||
         !is_none(embi_style_value(s, EMBI_PROP_mask_image));
    const embi_value *wc = embi_style_value(s, EMBI_PROP_will_change);
    st = st || wc_has(wc, "opacity") || wc_has(wc, "filter") || wc_has(wc, "isolation") ||
         wc_has(wc, "mix-blend-mode") || wc_has(wc, "clip-path") || wc_has(wc, "mask") || wc_has(wc, "backdrop-filter") ||
         wc_has(wc, "mask-image");
    o->st_common = st;
    o->st_block = !is_none(embi_style_value(s, EMBI_PROP_transform)) ||
                  !is_none(embi_style_value(s, EMBI_PROP_perspective)) ||
                  !is_none(embi_style_value(s, EMBI_PROP_translate)) ||
                  !is_none(embi_style_value(s, EMBI_PROP_rotate)) ||
                  !is_none(embi_style_value(s, EMBI_PROP_scale)) || preserves_3d(s) || backface_hidden(s) ||
                  wc_has(wc, "transform") || wc_has(wc, "perspective") || contain_creates(s);
    o->cb_block = cb_all_style(s);
    o->xf = embi_lay_has_transform(s);
    {
        const embi_value *reset = embi_style_value(s, EMBI_PROP_counter_reset);
        const embi_value *set = embi_style_value(s, EMBI_PROP_counter_set);
        const embi_value *inc = embi_style_value(s, EMBI_PROP_counter_increment);
        o->ctr_none = (reset == nullptr || reset->kind == EMBI_V_KEYWORD) &&
                      (set == nullptr || set->kind == EMBI_V_KEYWORD) &&
                      (inc == nullptr || inc->kind == EMBI_V_KEYWORD);
    }
    o->ub = kwp(s, EMBI_PROP_unicode_bidi);
    o->va = kwp(s, EMBI_PROP_vertical_align);
    o->rtl = kwp(s, EMBI_PROP_direction) == EMBI_KW_rtl;
    {
        static const embi_prop_id props[8] = {
            EMBI_PROP_margin_left,  EMBI_PROP_margin_right,  EMBI_PROP_margin_top,  EMBI_PROP_margin_bottom,
            EMBI_PROP_padding_left, EMBI_PROP_padding_right, EMBI_PROP_padding_top, EMBI_PROP_padding_bottom};
        for (int i = 0; i < 8 && !o->edges_vary; i++) {
            const embi_value *v = embi_style_value(s, props[i]);
            o->edges_vary = v != nullptr &&
                            (v->kind == EMBI_V_PERCENT || v->kind == EMBI_V_CALC || v->kind == EMBI_V_FUNC ||
                             (v->kind == EMBI_V_LENGTH && v->unit >= EMBI_U_CQW && v->unit <= EMBI_U_CQMAX));
        }
    }
    o->ws = embi_lay_ws_mode(s);
    o->tt = embi_lay_transform(s);
    /* W6b: the facts that keep a box out of the chain of a partial relayout (incr.c) */
    {
        static const embi_prop_id szp[6] = {EMBI_PROP_width,      EMBI_PROP_min_width,  EMBI_PROP_max_width,
                                            EMBI_PROP_height,     EMBI_PROP_min_height, EMBI_PROP_max_height};
        bool dep = false;
        for (int i = 0; i < 6 && !dep; i++) {
            const embi_value *v = embi_style_value(s, szp[i]);
            dep = v != nullptr && (v->kind == EMBI_V_FUNC ||
                                   (v->kind == EMBI_V_KEYWORD && v->kw != EMBI_KW_auto && v->kw != EMBI_KW_none));
        }
        const embi_value *ar = embi_style_value(s, EMBI_PROP_aspect_ratio);
        dep = dep || (ar != nullptr && !(ar->kind == EMBI_V_KEYWORD && ar->kw == EMBI_KW_auto));
        const embi_value *cc = embi_style_value(s, EMBI_PROP_column_count);
        const embi_value *cw = embi_style_value(s, EMBI_PROP_column_width);
        dep = dep || (cc != nullptr && !(cc->kind == EMBI_V_KEYWORD && cc->kw == EMBI_KW_auto));
        dep = dep || (cw != nullptr && !(cw->kind == EMBI_V_KEYWORD && cw->kw == EMBI_KW_auto));
        uint16_t wm = kwp(s, EMBI_PROP_writing_mode);
        dep = dep || (wm != 0 && wm != EMBI_KW_horizontal_tb);
        uint16_t ct = kwp(s, EMBI_PROP_container_type);
        dep = dep || (ct != 0 && ct != EMBI_KW_normal);
        uint16_t cont = kwp(s, EMBI_PROP_continue);
        o->mc = (cc != nullptr && !(cc->kind == EMBI_V_KEYWORD && cc->kw == EMBI_KW_auto)) ||
                (cw != nullptr && !(cw->kind == EMBI_V_KEYWORD && cw->kw == EMBI_KW_auto));
        o->clamp = (cont == EMBI_KW_collapse || cont == EMBI_KW_discard) && !o->mc; /* a multicol container: collapse acts as normal */
        o->incr_dep = dep || o->clamp;
        for (int i = 0; i < 6 && !o->calc_size; i++) {
            o->calc_size = embi_lay_is_calc_size(embi_style_value(s, szp[i]));
        }
    }
}

const embi_lay_ssum *embi_lay_ssum_of(const embi_lctx *c, const embi_style *s) {
    embi_lay_doc *ld = c->ld;
    if (ld->stab_n == 0) {
        ld->stab = embi_calloc(EMBI_SSUM_N, sizeof *ld->stab);
        if (ld->stab == nullptr) {
            static _Thread_local embi_lay_ssum spare;
            ssum_compute(&spare, s);
            return &spare;
        }
        ld->stab_n = EMBI_SSUM_N;
    }
    uint32_t mask = ld->stab_n - 1u;
    uint32_t i = (uint32_t)((((uint64_t)(uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    for (;;) {
        embi_lay_ssum *e = &ld->stab[i];
        if (e->key == s && s != nullptr) {
            return e;
        }
        if (e->key == nullptr) {
            break;
        }
        i = (i + 1u) & mask;
    }
    if ((ld->stab_cnt + 1u) * 2u > ld->stab_n) {
        /* Double the table (rare: a page has a few hundred styles at most). The old entries are re-inserted. */
        uint32_t n2 = ld->stab_n * 2u;
        embi_lay_ssum *t2 = embi_calloc(n2, sizeof *t2);
        if (t2 == nullptr) {
            /* Out of memory: answer from a spare instead of filling past half load, so the probe loop
             * above always finds an empty slot. */
            static _Thread_local embi_lay_ssum spare_oom;
            ssum_compute(&spare_oom, s);
            return &spare_oom;
        }
        {
            for (uint32_t j = 0; j < ld->stab_n; j++) {
                if (ld->stab[j].key == nullptr) {
                    continue;
                }
                uint32_t k = (uint32_t)((((uint64_t)(uintptr_t)ld->stab[j].key >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & (n2 - 1u);
                while (t2[k].key != nullptr) {
                    k = (k + 1u) & (n2 - 1u);
                }
                t2[k] = ld->stab[j];
            }
            embi_free(ld->stab, (size_t)ld->stab_n * sizeof *ld->stab);
            ld->stab = t2;
            ld->stab_n = n2;
            mask = n2 - 1u;
            i = (uint32_t)((((uint64_t)(uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull) >> 40) & mask;
            while (ld->stab[i].key != nullptr) {
                i = (i + 1u) & mask;
            }
        }
    }
    embi_lay_ssum *e = &ld->stab[i];
    ssum_compute(e, s);
    if (s != nullptr) {
        ld->stab_cnt++;
    }
    return e;
}

/* The style whose non-inherited properties a box reads (embi_lay_prop): the initial style for an anonymous
 * box. */
static const embi_style *eff_style(const embi_lctx *c, const embi_box *b) {
    return (b->flags & EMBI_BF_ANON) != 0 || b->style == nullptr ? c->initial : b->style;
}

static bool cb_all(const embi_lctx *c, const embi_box *b) {
    if (!has_style(b) || b->kind == EMBI_BK_INLINE) {
        return false;
    }
    return embi_lay_ssum_of(c, eff_style(c, b))->cb_block;
}

/* The direction of a box is rtl. */
static bool dir_rtl(const embi_lctx *c, const embi_box *b) {
    const embi_value *d = embi_lay_prop(c, b, EMBI_PROP_direction);
    return d != nullptr && d->kind == EMBI_V_KEYWORD && d->kw == EMBI_KW_rtl;
}

/* An inset (top, right, bottom or left) as px against pct, NaN when auto. */
static float inset_len(const embi_lctx *c, const embi_box *b, embi_prop_id p, float pct) {
    const embi_value *v = embi_lay_prop(c, b, p);
    if (embi_lay_is_auto(v)) {
        return NAN;
    }
    return embi_lay_len(c, b, v, pct);
}

/* ------------------------------------------------------------------ containing blocks */

/* The containing block box of an out-of-flow box (nullptr: the initial containing block). */
static const embi_box *cb_box(const embi_lctx *c, const embi_box *k, bool fixed) {
    for (const embi_box *a = k->parent; a != nullptr; a = a->parent) {
        if (!has_style(a)) {
            continue;
        }
        if (cb_all(c, a) || (a->flags2 & EMBI_BF2_SVGFO) != 0) {
            return a; /* R4c: a foreignObject is the containing block of all its descendants */
        }
        if (!fixed && is_positioned(pos_of(c, a))) {
            return a;
        }
    }
    return nullptr;
}

const embi_box *embi_lay_abspos_cb(const embi_lctx *c, const embi_box *k) {
    return cb_box(c, k, pos_of(c, k) == EMBI_KW_fixed);
}

/* The root of the box tree of b (its direction is the initial containing block's). */
static const embi_box *root_of(const embi_box *b) {
    while (b->parent != nullptr) {
        b = b->parent;
    }
    return b;
}

/* ------------------------------------------------------------------ frames */

/* The origin of f's border box relative to the border box of its ancestor anc, by summing the
 * positions on the way. False when anc is not an ancestor of f (or f itself). */
static bool origin_in(const embi_frag *f, const embi_frag *anc, float *x, float *y) {
    float ax = 0, ay = 0;
    for (const embi_frag *p = f; p != anc; p = p->parent) {
        if (p == nullptr) {
            return false;
        }
        ax += p->x;
        ay += p->y;
    }
    *x = ax;
    *y = ay;
    return true;
}

/* ------------------------------------------------------------------ inline containing blocks */

/* The key of an inline box in the inline cache: its element (and pseudo-element), so that the pieces of an inline split
 * around a block share their first and last fragments (CSS 2.1 section 10.1 item 4: the first and last boxes the element
 * generates); the box itself for an anonymous one. */
static uint64_t icache_key(const embi_box *b) {
    if (b->el == nullptr) {
        return (uint64_t)(uintptr_t)b;
    }
    return ((uint64_t)(uintptr_t)b->el << 4 | (uint64_t)(b->pe & 15u)) | (1ull << 63);
}

static void icache_note(struct embi_pos *p, embi_frag *k) {
    uint64_t key = icache_key(k->box);
    uint64_t i = 0;
    if (embi_hashmap_get(&p->icmap, key, &i)) {
        p->ic.data[i].last = k;
        return;
    }
    embi_icache_ent e = {k->box, k, k};
    embi_vec_push(&p->ic, e);
    (void)embi_hashmap_put(&p->icmap, key, p->ic.len - 1u);
}

/* Notes the inline box fragments of the subtree of f, in tree order. */
static void icache_walk(struct embi_pos *p, embi_frag *f) {
    if (f->kind == EMBI_FK_BOX && f->box != nullptr && f->box->kind == EMBI_BK_INLINE) {
        icache_note(p, f);
    }
    for (embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        icache_walk(p, k);
    }
}

/* The first and last fragments of inline box b in the tree (built on first use from the whole tree
 * icb, then kept up to date as the placed subtrees are added). False when b has none. */
static bool inline_frags(struct embi_pos *p, embi_frag *icb, const embi_box *b, embi_frag **ff,
                         embi_frag **lf) {
    if (!p->ic_ready) {
        icache_walk(p, icb);
        p->ic_ready = true;
    }
    uint64_t i = 0;
    if (!embi_hashmap_get(&p->icmap, icache_key(b), &i)) {
        return false;
    }
    *ff = p->ic.data[i].first;
    *lf = p->ic.data[i].last;
    return true;
}

/* ------------------------------------------------------------------ recording */

void embi_lay_abspos_at(embi_lctx *c, embi_box *k, embi_box *sbox, float sx, float sy,
                        const float *cb) {
    if (c->pos == nullptr) {
        c->pos = embi_calloc(1, sizeof *c->pos);
        if (c->pos == nullptr) {
            return;
        }
    }
    struct embi_pos *p = c->pos;
    embi_pos_rec r;
    memset(&r, 0, sizeof r);
    r.box = k;
    r.sbox = sbox;
    r.sx = sx;
    r.sy = sy;
    if (cb != nullptr) {
        r.has_cb = true;
        memcpy(r.cb, cb, sizeof r.cb);
    }
    uint32_t idx = p->recs.len;
    embi_vec_push(&p->recs, r);
    (void)embi_hashmap_put(&p->last, (uint64_t)(uintptr_t)k, idx);
}

void embi_lay_pos_replay(embi_lctx *c, uint32_t from, uint32_t to) {
    struct embi_pos *p = c->pos;
    if (p == nullptr || to > p->recs.len) {
        return;
    }
    for (uint32_t i = from; i < to; i++) {
        embi_pos_rec r = p->recs.data[i];
        uint32_t idx = p->recs.len;
        embi_vec_push(&p->recs, r);
        (void)embi_hashmap_put(&p->last, (uint64_t)(uintptr_t)r.box, idx);
    }
}

void embi_lay_abspos_flex(embi_lctx *c, embi_box *k, embi_box *sbox, const float cont[4],
                          const float frac[2], const bool flip[2]) {
    embi_lay_abspos_at(c, k, sbox, 0.0f, 0.0f, nullptr);
    struct embi_pos *p = c->pos;
    if (p == nullptr || p->recs.len == 0 || p->recs.data[p->recs.len - 1].box != k) {
        return;
    }
    embi_pos_rec *r = &p->recs.data[p->recs.len - 1];
    r->flex = true;
    memcpy(r->fcont, cont, sizeof r->fcont);
    memcpy(r->ffrac, frac, sizeof r->ffrac);
    memcpy(r->fflip, flip, sizeof r->fflip);
}

void embi_lay_abspos_flip(embi_lctx *c, const embi_box *k) {
    struct embi_pos *p = c->pos;
    if (p == nullptr || p->recs.len == 0 || p->recs.data[p->recs.len - 1].box != k) {
        return;
    }
    p->recs.data[p->recs.len - 1].sflip = true;
}

void embi_lay_abspos_grid(embi_lctx *c, embi_box *k, embi_box *sbox, const float cb[4]) {
    embi_lay_abspos_at(c, k, sbox, 0.0f, 0.0f, cb);
}

/* The out-of-flow children of an inline formatting context: those in its inline content, at any
 * depth of inline boxes. Their static position is recorded by the inline layout (inline.c
 * abs_static), which knows the line placement; the content start is the fallback. */
static void ifc_abspos(embi_lctx *c, embi_box *b, float sx, float sy) {
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (k->flags & EMBI_BF_ABSPOS) {
            embi_lay_abspos_at(c, k, b, sx, sy, nullptr);
        } else if (k->kind == EMBI_BK_INLINE) {
            ifc_abspos(c, k, sx, sy);
        }
    }
}

void embi_lay_abspos_ifc(embi_lctx *c, embi_box *b, float sx, float sy) {
    ifc_abspos(c, b, sx, sy);
}

/* ------------------------------------------------------------------ relative and sticky */

/* The offsets of a relatively positioned box (CSS Position 3 section 3.2, CSS 2.1 section 9.4.3).
 * Top wins over bottom. Left wins over right in ltr, and right wins in rtl, where the containing
 * block's direction decides. Percentages resolve against the content box of the nearest block
 * container (the scrollbar gutters are not part of it), the height only when it is not auto. */
static void rel_offset(const embi_lctx *c, const embi_box *a, float *dx, float *dy) {
    const embi_box *cb = a->parent;
    while (cb != nullptr && cb->kind == EMBI_BK_INLINE) {
        cb = cb->parent;
    }
    bool rtl = cb != nullptr && dir_rtl(c, cb);
    float cbw = NAN, cbh = NAN;
    if (cb != nullptr && cb->frag != nullptr) {
        const embi_frag *F = cb->frag;
        cbw = F->w - embi_frag_bdr(F, 0) - embi_frag_bdr(F, 2) - embi_frag_pad(F, 0) - embi_frag_pad(F, 2) - embi_frag_scr(F, 0);
        if (!embi_lay_is_auto(embi_lay_prop(c, cb, EMBI_PROP_height)) || (F->flags & EMBI_FF_DEF_H) != 0) {
            cbh = F->h - embi_frag_bdr(F, 1) - embi_frag_bdr(F, 3) - embi_frag_pad(F, 1) - embi_frag_pad(F, 3) - embi_frag_scr(F, 1);
        }
    }
    float L = inset_len(c, a, EMBI_PROP_left, cbw);
    float Rt = inset_len(c, a, EMBI_PROP_right, cbw);
    float T = inset_len(c, a, EMBI_PROP_top, cbh);
    float B = inset_len(c, a, EMBI_PROP_bottom, cbh);
    if (!isnan(L) && !isnan(Rt)) {
        *dx = rtl ? -Rt : L;
    } else {
        *dx = !isnan(L) ? L : (!isnan(Rt) ? -Rt : 0.0f);
    }
    if (!isnan(T) && !isnan(B)) {
        *dy = T;
    } else {
        *dy = !isnan(T) ? T : (!isnan(B) ? -B : 0.0f);
    }
}

/* The offset that the relative boxes give to a fragment of box y. An out-of-flow box takes none. An
 * inline-level box takes the offsets of its inline ancestors too (they move their content, and each
 * fragment of the content is a separate fragment of the line); a block-level box moves with its own
 * fragment only, since its children are positioned in it. */
static void frag_shift(const embi_lctx *c, const embi_box *y, float *dx, float *dy) {
    *dx = 0;
    *dy = 0;
    if (y->flags & EMBI_BF_ABSPOS) {
        return;
    }
    if ((y->flags2 & EMBI_BF2_PPSHIFT0) != 0) {
        return;
    }
    if (y->kind == EMBI_BK_TEXT && y->parent != nullptr && (y->parent->flags2 & EMBI_BF2_PPSHIFT0) != 0) {
        /* W3q: a text run in a box that nothing shifts (the parent's walk covered the same inline ancestors, and a text run is
         * not inline-level in the sense of the block part below) */
        ((embi_box *)(uintptr_t)y)->flags2 |= EMBI_BF2_PPSHIFT0;
        return;
    }
    bool any_rel = false;
    for (const embi_box *a = y; a != nullptr; a = a->parent) {
        if (a != y && a->kind != EMBI_BK_INLINE) {
            break;
        }
        if (has_style(a) && kw_of(c, a, EMBI_PROP_position) == EMBI_KW_relative) {
            float x = 0, yy = 0;
            rel_offset(c, a, &x, &yy);
            *dx += x;
            *dy += yy;
            any_rel = true;
        }
    }
    if (y->kind != EMBI_BK_INLINE && (y->flags & (EMBI_BF_FLOAT | EMBI_BF_INLINE_LEVEL)) == 0 && y->el != nullptr) {
        /* A block-level box inside an inline box (the inline was split around it) moves with that inline's
         * relative offset too (CSS 2.1 section 9.4.3): the inline boxes between it and its block parent are found
         * through the element tree. */
        const embi_element *stop = y->parent != nullptr ? y->parent->el : nullptr;
        const embi_node *n = y->el->base.parent;
        while (n != nullptr && embi_is_element(n) && (const embi_element *)n != stop) {
            const embi_element *ae = (const embi_element *)n;
            /* The element's box is its first piece, or the block itself when the inline holds only blocks: its style
             * decides. A stand-in inline box with that style and y's block parent gives rel_offset its containing
             * block. */
            const embi_style *st = embi_style_get(c->style, (embi_element *)(uintptr_t)ae, EMBI_PE_NONE);
            const embi_value *dv = st != nullptr ? embi_style_value(st, EMBI_PROP_display) : nullptr;
            if (dv == nullptr || dv->kind != EMBI_V_KEYWORD || dv->kw != EMBI_KW_inline) {
                break;
            }
            const embi_value *pv = embi_style_value(st, EMBI_PROP_position);
            if (pv != nullptr && pv->kind == EMBI_V_KEYWORD && pv->kw == EMBI_KW_relative) {
                embi_box tmp;
                memset(&tmp, 0, sizeof tmp);
                tmp.kind = EMBI_BK_INLINE;
                tmp.style = st;
                tmp.el = (embi_element *)(uintptr_t)ae;
                tmp.parent = y->parent;
                float x = 0, yy = 0;
                rel_offset(c, &tmp, &x, &yy);
                *dx += x;
                *dy += yy;
                any_rel = true;
            }
            n = n->parent;
        }
    }
    if (!any_rel) {
        ((embi_box *)(uintptr_t)y)->flags2 |= EMBI_BF2_PPSHIFT0;
    }
}

/* The static position of an out-of-flow box k in its static container sbox moves with the relative
 * offsets of the inline boxes between them (the static position is inside those boxes' content). */
static void inline_static(const embi_lctx *c, const embi_box *k, const embi_box *sbox, float *sx,
                          float *sy) {
    for (const embi_box *a = k->parent; a != nullptr && a != sbox && a->kind == EMBI_BK_INLINE;
         a = a->parent) {
        if (has_style(a) && kw_of(c, a, EMBI_PROP_position) == EMBI_KW_relative) {
            float dx = 0, dy = 0;
            rel_offset(c, a, &dx, &dy);
            *sx += dx;
            *sy += dy;
        }
    }
}

/* The sticky data of fragment k. Its containing block is the content box of the nearest
 * block-level box fragment above it (anonymous boxes are skipped). For an inline-level box reached
 * through line fragments (and inline box fragments), their positions are summed into (ox, oy), so
 * the border box is known in that block's frame. */
static void store_sticky(embi_lctx *c, embi_frag *k, embi_frag *icb) {
    embi_frag *P = k->parent;
    float lx = 0, ly = 0;
    while (P != nullptr &&
           (P->kind == EMBI_FK_LINE ||
            (P->box != nullptr &&
             (P->box->kind == EMBI_BK_INLINE || (P->box->flags & EMBI_BF_ANON))))) {
        lx += P->x;
        ly += P->y;
        P = P->parent;
    }
    if (P == nullptr) {
        return;
    }
    embi_frag *SC = P;
    while (SC != nullptr && (SC->flags & EMBI_FF_SCROLL) == 0) {
        SC = SC->parent;
    }
    if (SC == nullptr) {
        SC = icb;
    }
    float ox = 0, oy = 0;
    if (!origin_in(P, SC, &ox, &oy)) {
        return;
    }
    struct embi_sticky *s = embi_lay_alloc(&c->ld->fa, sizeof *s, alignof(struct embi_sticky));
    if (s == nullptr) {
        return;
    }
    s->ox = lx;
    s->oy = ly;
    /* The scrollport in SC's frame (inside the scrollbar gutters), moved into P's frame. */
    float vx0 = embi_frag_bdr(SC, 0) - ox, vy0 = embi_frag_bdr(SC, 1) - oy;
    float vx1 = SC->w - embi_frag_bdr(SC, 2) - embi_frag_scr(SC, 0) - ox, vy1 = SC->h - embi_frag_bdr(SC, 3) - embi_frag_scr(SC, 1) - oy;
    s->view[0] = vx0;
    s->view[1] = vy0;
    s->view[2] = vx1;
    s->view[3] = vy1;
    /* The containing block of a sticky box is its parent's content box (in P's frame). */
    s->cont[0] = embi_frag_bdr(P, 0) + embi_frag_pad(P, 0);
    s->cont[1] = embi_frag_bdr(P, 1) + embi_frag_pad(P, 1);
    s->cont[2] = P->w - embi_frag_bdr(P, 2) - embi_frag_pad(P, 2) - embi_frag_scr(P, 0);
    s->cont[3] = P->h - embi_frag_bdr(P, 3) - embi_frag_pad(P, 3) - embi_frag_scr(P, 1);
    if (P == SC) {
        /* The parent is the scroll container: its content box extends over the scrollable overflow (which ends with
         * the end padding), so the box can stick anywhere in the scrolled content (position-sticky-top-and-bottom-003). */
        float pov[4];
        embi_frag_ov(P, pov);
        s->cont[2] = fmaxf(s->cont[2], pov[2] - embi_frag_pad(P, 2));
        s->cont[3] = fmaxf(s->cont[3], pov[3] - embi_frag_pad(P, 3));
    }
    /* Percentages of sticky insets resolve against the scrollport (CSS Position 3 section 3.4). */
    float pw = vx1 - vx0, ph = vy1 - vy0;
    s->inset[0] = inset_len(c, k->box, EMBI_PROP_top, ph);
    s->inset[1] = inset_len(c, k->box, EMBI_PROP_right, pw);
    s->inset[2] = inset_len(c, k->box, EMBI_PROP_bottom, ph);
    s->inset[3] = inset_len(c, k->box, EMBI_PROP_left, pw);
    float cw = s->cont[2] - s->cont[0];
    s->mg[0] = nz(inset_len(c, k->box, EMBI_PROP_margin_top, cw));
    s->mg[1] = nz(inset_len(c, k->box, EMBI_PROP_margin_right, cw));
    s->mg[2] = nz(inset_len(c, k->box, EMBI_PROP_margin_bottom, cw));
    s->mg[3] = nz(inset_len(c, k->box, EMBI_PROP_margin_left, cw));
    s->scroll = SC;
    embi_fext *x = embi_lay_frag_ext(c->ld, k);
    if (x != nullptr) {
        x->sticky = s;
    }
}

/* One axis of the sticky offset (CSS Position 3 section 3.4). The border edges e0 and e1 are
 * compared with the sticky view rectangle (v0, v1) shrunk by the insets. The offset is limited so
 * that the margin box (the position box, from the specified margins: the used margin that
 * over-constraint gives an unset side is not a margin) stays inside the containing block (c0, c1). The second
 * inset applies after the first, within the same limits. */
static float sticky_axis(float e0, float e1, float in0, float in1, float v0, float v1, float c0,
                         float c1, float mg0, float mg1) {
    float room_fwd = fmaxf(c1 - e1 - mg1, 0.0f); /* the most the box can move toward the end */
    float room_back = fmaxf(e0 - c0 - mg0, 0.0f);
    float d = 0;
    if (!isnan(in0) && e0 + d < v0 + in0) {
        d = v0 + in0 - e0;
        d = fminf(d, room_fwd);
        d = fmaxf(d, -room_back);
    }
    if (!isnan(in1) && e1 + d > v1 - in1) {
        d = v1 - in1 - e1;
        d = fminf(d, room_fwd);
        d = fmaxf(d, -room_back);
    }
    return d;
}

void embi_lay_sticky_offset(const embi_frag *f, float *dx, float *dy) {
    *dx = 0;
    *dy = 0;
    const struct embi_sticky *s = embi_frag_sticky(f);
    if (s == nullptr) {
        return;
    }
    float sx = s->scroll != nullptr ? embi_frag_sx(s->scroll) : 0.0f;
    float sy = s->scroll != nullptr ? embi_frag_sy(s->scroll) : 0.0f;
    float x = f->x + s->ox, y = f->y + s->oy;
    *dy = sticky_axis(y, y + f->h, s->inset[0], s->inset[2], s->view[1] + sy, s->view[3] + sy,
                      s->cont[1], s->cont[3], s->mg[0], s->mg[2]);
    *dx = sticky_axis(x, x + f->w, s->inset[3], s->inset[1], s->view[0] + sx, s->view[2] + sx,
                      s->cont[0], s->cont[2], s->mg[3], s->mg[1]);
}

bool embi_lay_frag_vp_fixed(const embi_frag *f) {
    if (f == nullptr || f->kind != EMBI_FK_BOX || f->box == nullptr || f->box->style == nullptr ||
        f->parent == nullptr || f->parent->parent != nullptr || (f->box->flags & EMBI_BF_ROOT) != 0) {
        return false; /* the root element's box is painted with the viewport's context, whatever its position */
    }
    const embi_value *v = embi_style_value(f->box->style, EMBI_PROP_position);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_fixed;
}

/* ------------------------------------------------------------------ stacking flags */

/* The stacking flags of a box fragment (CSS 2.1 Appendix E, and the properties of CSS Position 3,
 * Transforms, Filter Effects, Compositing, Masking and Containment that create a stacking context),
 * and z-index on positioned boxes and on flex and grid items. Inline boxes (non-replaced) take the
 * properties that apply to them: transform, perspective, and the containment and size properties do
 * not apply to them (CSS Transforms 1 and CSS Containment 2 "Applies to"). */
static void set_flags(const embi_lctx *c, embi_frag *f) {
    const embi_box *b = f->box;
    uint8_t keep = f->flags & (EMBI_FF_SCROLL | EMBI_FF_CLIP | EMBI_FF_DEF_H | EMBI_FF_TABLE |
                                EMBI_FF_HIDE_EMPTY | EMBI_FF_EXT);
    embi_fext *x0 = embi_frag_ext(f);
    if (x0 != nullptr) {
        x0->z_index = 0;
    }
    if (b == nullptr || !has_style(b) || (b->flags2 & EMBI_BF2_PPFLAGS0) != 0) {
        f->flags = keep;
        return;
    }
    bool inl = b->kind == EMBI_BK_INLINE;
    bool zset = false;
    const embi_lay_ssum sm = *embi_lay_ssum_of(c, eff_style(c, b));
    uint16_t pos = sm.pos;
    bool in_flex_grid = b->parent != nullptr &&
                        (b->parent->kind == EMBI_BK_FLEX || b->parent->kind == EMBI_BK_GRID);
    bool zauto = sm.zauto;
    if (!zauto && (is_positioned(pos) || in_flex_grid)) {
        int32_t zi = sm.zi;
        if (zi != 0) {
            embi_fext *x = embi_lay_frag_ext(c->ld, f);
            if (x != nullptr) {
                x->z_index = zi;
            }
            zset = true;
        }
    }

    bool st = (b->flags & EMBI_BF_ROOT) != 0 || pos == EMBI_KW_fixed || pos == EMBI_KW_sticky ||
              (is_positioned(pos) && !zauto) || (in_flex_grid && !zauto);
    st = st || sm.st_common;
    /* Inline boxes (non-replaced) take the properties that apply to them: transform, perspective, and the
     * containment and size properties do not apply to them. */
    if (!inl) {
        st = st || sm.st_block;
    }
    bool cbf = !inl && sm.cb_block;
    if ((b->flags2 & EMBI_BF2_SVGFO) != 0) {
        st = cbf = true; /* R4c: a foreignObject box is painted as a context by the SVG walk */
    }
    /* The side record may have been made above (z-index): its flag is not in keep. */
    f->flags = keep | (f->flags & EMBI_FF_EXT) | (st ? EMBI_FF_STACKING : 0) | (cbf ? EMBI_FF_CB_FIXED : 0);
    if (!st && !cbf && !zset) {
        ((embi_box *)(uintptr_t)b)->flags2 |= EMBI_BF2_PPFLAGS0;
    }
}

/* The final pass over the flow (and over each placed subtree, before it is appended): the stacking
 * flags, and the relative offsets. A shifted fragment moves its children with it (they are
 * positioned in it), and its ancestors' overflow grows to cover it. */
static void prep_node(const embi_lctx *c, embi_frag *k) {
    if (k->kind == EMBI_FK_BOX && k->box != nullptr) {
        set_flags(c, k);
    }
    if ((k->kind == EMBI_FK_BOX || k->kind == EMBI_FK_TEXT) && k->box != nullptr) {
        float dx = 0, dy = 0;
        frag_shift(c, k->box, &dx, &dy);
        if (dx != 0 || dy != 0) {
            k->x += dx;
            k->y += dy;
            for (embi_frag *p = k; p->parent != nullptr; p = p->parent) {
                embi_lay_ov_add_child(p->parent, p);
            }
        }
    }
    if (c->ld->reuse_on && embi_hashmap_len(&c->ld->reusedmap) != 0) {
        uint64_t rv = 0;
        if (embi_hashmap_get(&c->ld->reusedmap, (uint64_t)(uintptr_t)k, &rv)) {
            return; /* W6b: a fragment kept from the last layout: this pass already ran over what is inside it */
        }
    }
    for (embi_frag *ch = embi_frag_first(k); ch != nullptr; ch = ch->next) {
        prep_node(c, ch);
    }
}

/* W6b: the final pass over a fragment subtree that is not attached yet (a partial relayout), and the offset the relative
 * boxes give a fragment: the same functions the full pass uses. */
void embi_lay_prep_subtree(embi_lctx *c, embi_frag *f) {
    prep_node(c, f);
}

/* W6e: the stacking flags of a block-level box recomputed in place from its (refreshed) style, after a change that only
 * flips the stacking context (opacity crossing 1). False when the box does not have exactly the one fragment this can
 * patch; the caller lays the kept tree out again then. */
bool embi_lay_restack(const embi_lctx *c, embi_box *b) {
    embi_frag *f = b->frag;
    if (f == nullptr || f->box != b || f->kind != EMBI_FK_BOX || f->inl != 0 ||
        (b->kind != EMBI_BK_BLOCK && b->kind != EMBI_BK_FLEX && b->kind != EMBI_BK_GRID) ||
        (b->flags & (EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC | EMBI_BF_REPLACED)) != 0) {
        return false;
    }
    b->flags2 &= (uint16_t)~EMBI_BF2_PPFLAGS0;
    set_flags(c, f);
    return true;
}

uint32_t embi_lay_pos_count(const embi_lctx *c) {
    return c->pos != nullptr ? c->pos->recs.len : 0;
}

void embi_lay_pos_discard(embi_lctx *c) {
    struct embi_pos *p = c->pos;
    if (p != nullptr) {
        embi_hashmap_free(&p->last);
        embi_hashmap_free(&p->icmap);
        embi_vec_free(&p->recs);
        embi_vec_free(&p->ic);
        embi_free(p, sizeof *p);
        c->pos = nullptr;
    }
}

void embi_lay_frag_shift(const embi_lctx *c, const embi_box *b, float *dx, float *dy) {
    frag_shift(c, b, dx, dy);
}

static void walk_sticky(embi_lctx *c, embi_frag *f, embi_frag *icb) {
    for (embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_BOX && k->box != nullptr && k->parent != nullptr &&
            (k->box->flags2 & EMBI_BF2_PPFLAGS0) == 0 && pos_of(c, k->box) == EMBI_KW_sticky) {
            store_sticky(c, k, icb);
        }
        walk_sticky(c, k, icb);
    }
}

/* ------------------------------------------------------------------ placement */

/* The border-box start edge of an out-of-flow child of a flex container on an axis (0: x, 1: y), in
 * the frame of the static container (CSS Flexbox 1 section 4.1: the position of the sole item). bs is
 * the border-box size, and ms and me the margins at the logical start and end. The fraction ffrac of
 * the free space moves the child from the logical start, which is the far edge when fflip. */
static float flex_axis(const embi_pos_rec *r, int axis, float bs, float ms, float me) {
    float c0 = r->fcont[axis], c1 = r->fcont[axis + 2];
    float free = (c1 - c0) - bs - ms - me;
    float f = r->ffrac[axis];
    if (r->fflip[axis]) {
        return c1 - ms - bs - f * free;
    }
    return c0 + ms + f * free;
}

/* Solves the insets of an out-of-flow box and places it (CSS 2.1 10.3.7 and 10.6.4). Its used size
 * comes from its own layout in the available size the insets leave. The equations follow the
 * direction of the containing block; the static position's edge follows the static container's
 * direction (CSS 2.1 10.3.7 and CSS Position 3 section 5). */
static void place_one(embi_lctx *c, const embi_pos_rec *r, embi_frag *icb) {
    embi_box *k = r->box;
    /* A null static container is the initial containing block (an absolutely positioned root element). */
    if (k->frag != nullptr || (r->sbox != nullptr && r->sbox->frag == nullptr)) {
        return;
    }
    struct embi_pos *p = c->pos;
    embi_frag *sf = r->sbox != nullptr ? r->sbox->frag : icb;
    bool fixed = pos_of(c, k) == EMBI_KW_fixed;

    /* The containing block: its frame F, its padding box R in that frame, and its direction. */
    embi_frag *F = icb;
    float R[4] = {0, 0, icb->w, icb->h};
    bool crtl;
    if (r->has_cb) {
        F = sf;
        memcpy(R, r->cb, sizeof R);
        /* A grid area edge that is NaN is the padding box edge (CSS Grid 2 section 9.2). */
        if (isnan(R[0])) {
            R[0] = embi_frag_bdr(sf, 0);
        }
        if (isnan(R[1])) {
            R[1] = embi_frag_bdr(sf, 1);
        }
        if (isnan(R[2])) {
            R[2] = sf->w - embi_frag_bdr(sf, 2) - embi_frag_scr(sf, 0);
        }
        if (isnan(R[3])) {
            R[3] = sf->h - embi_frag_bdr(sf, 3) - embi_frag_scr(sf, 1);
        }
        crtl = dir_rtl(c, r->sbox != nullptr ? r->sbox : root_of(k));
    } else {
        const embi_box *cbb = cb_box(c, k, fixed);
        crtl = dir_rtl(c, cbb != nullptr ? cbb : root_of(k));
        if (cbb != nullptr && cbb->kind == EMBI_BK_INLINE) {
            embi_frag *ff = nullptr, *lf = nullptr;
            if (inline_frags(p, icb, cbb, &ff, &lf)) {
                const embi_box *bb = cbb->parent;
                while (bb != nullptr && bb->kind == EMBI_BK_INLINE) {
                    bb = bb->parent;
                }
                float fx = 0, fy = 0, lx = 0, ly = 0;
                embi_frag *BF = bb != nullptr ? bb->frag : nullptr;
                if (BF == nullptr || !origin_in(ff, BF, &fx, &fy) || !origin_in(lf, BF, &lx, &ly)) {
                    BF = icb;
                    (void)origin_in(ff, icb, &fx, &fy);
                    (void)origin_in(lf, icb, &lx, &ly);
                }
                F = BF;
                /* From the start edge of the first fragment's padding box to the end edge of the last one's,
                 * swapped when they cross (CSS 2.1 section 10.1 item 4, the inline box's direction picks the
                 * edges; Chromium swaps a negative extent), and the bounding box of the two on the block axis. */
                float l0 = fx + embi_frag_bdr(ff, 0), r0 = fx + ff->w - embi_frag_bdr(ff, 2);
                float l1 = lx + embi_frag_bdr(lf, 0), r1 = lx + lf->w - embi_frag_bdr(lf, 2);
                R[0] = crtl ? l1 : l0;
                R[2] = crtl ? r0 : r1;
                if (R[2] < R[0]) {
                    float t = R[0];
                    R[0] = R[2];
                    R[2] = t;
                }
                R[1] = fminf(fy + embi_frag_bdr(ff, 1), ly + embi_frag_bdr(lf, 1));
                R[3] = fmaxf(fy + ff->h - embi_frag_bdr(ff, 3), ly + lf->h - embi_frag_bdr(lf, 3));
            }
        } else if (cbb != nullptr) {
            if (cbb->frag == nullptr) {
                return;
            }
            F = cbb->frag;
            R[0] = embi_frag_bdr(F, 0);
            R[1] = embi_frag_bdr(F, 1);
            R[2] = F->w - embi_frag_bdr(F, 2) - embi_frag_scr(F, 0);
            R[3] = F->h - embi_frag_bdr(F, 3) - embi_frag_scr(F, 1);
        }
    }

    /* The static margin edge in F's frame: the static container's position, then the relative
     * offsets of the inline boxes between them. */
    float ox = 0, oy = 0;
    if (!origin_in(sf, F, &ox, &oy)) {
        return;
    }
    float sx = r->sx, sy = r->sy;
    inline_static(c, k, r->sbox, &sx, &sy);
    float px = ox + sx;
    float py = oy + sy;
    bool srtl = dir_rtl(c, r->sbox != nullptr ? r->sbox : root_of(k)) != r->sflip;

    float W = fmaxf(R[2] - R[0], 0.0f);
    float H = fmaxf(R[3] - R[1], 0.0f);
    float L = inset_len(c, k, EMBI_PROP_left, W);
    float Rt = inset_len(c, k, EMBI_PROP_right, W);
    float T = inset_len(c, k, EMBI_PROP_top, H);
    float B = inset_len(c, k, EMBI_PROP_bottom, H);
    /* Insets that overlap make the inset-modified containing block negative: the weaker inset (the
     * end opposite the start, right in ltr and bottom) gives way (CSS Position 3 section 3.2). */
    if (!isnan(L) && !isnan(Rt) && W - L - Rt < 0) {
        if (crtl) {
            L = W - Rt;
        } else {
            Rt = W - L;
        }
    }
    if (!isnan(T) && !isnan(B) && H - T - B < 0) {
        B = H - T;
    }

    /* The layout of the box: the width is solved in the available width, the height is its content
     * height (CSS 2.1 10.6.4 for auto heights). */
    const embi_value *wv = embi_lay_prop(c, k, EMBI_PROP_width);
    bool wauto = embi_lay_is_auto(wv);
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, k, W, &bdr, &pad, &mar);
    bool lauto = isnan(L), rauto = isnan(Rt), tauto = isnan(T), bauto = isnan(B);
    bool mla = isnan(mar.l), mra = isnan(mar.r), mta = isnan(mar.t), mba = isnan(mar.b);
    float mlN = nz(mar.l), mrN = nz(mar.r), mtN = nz(mar.t), mbN = nz(mar.b);
    /* A replaced box keeps its intrinsic (or specified) size: insets never stretch it
     * (CSS 2.1 10.3.8, 10.6.5). */
    bool repl = k->kind == EMBI_BK_REPLACED;

    embi_cs cs;
    memset(&cs, 0, sizeof cs);
    cs.cb_w = W;
    cs.cb_h = H;
    cs.fit = EMBI_FIT_SHRINK;
    cs.avail_w = W;
    if (wauto) {
        if (!lauto && !rauto && k->kind != EMBI_BK_TABLE && !repl) {
            /* The width fills what the insets leave (CSS 2.1 10.3.7, no auto inset). A table is
             * shrink-to-fit instead (CSS Tables 3 section 3.5, its auto width is its content's),
             * and a replaced box keeps its own size. */
            cs.fit = EMBI_FIT_FILL;
            cs.avail_w = W - L - Rt; /* the layout takes the margins out of it */
        } else {
            /* Shrink-to-fit in what the insets leave; with both insets auto, from the static
             * position to the far edge of the containing block. */
            float avail;
            if (lauto && rauto) {
                avail = srtl ? px - R[0] : R[2] - px;
            } else {
                avail = W - (lauto ? 0.0f : L) - (rauto ? 0.0f : Rt);
            }
            cs.avail_w = avail; /* the layout takes the margins out of it */
        }
    }
    /* An auto height with both insets set fills what they leave (CSS 2.1 10.6.4): the auto margins
     * are zero here, and the border box is at least its edges. The used height is definite, so the
     * box is laid out at it (a flex container's items see it). */
    const embi_value *hv0 = embi_lay_prop(c, k, EMBI_PROP_height);
    /* height: stretch fills the inset-modified containing block, whose edge at an auto inset is the static position
     * (CSS Sizing 4 section 6.1, CSS Position 3 section 4.1; stretch/abspos-2). */
    bool hstretch = hv0 != nullptr && hv0->kind == EMBI_V_KEYWORD && hv0->kw == EMBI_KW_stretch;
    bool fill_h = ((embi_lay_is_auto(hv0) && !isnan(T) && !isnan(B)) || hstretch) && k->kind != EMBI_BK_TABLE && !repl;
    float fT = !isnan(T) ? T : fmaxf(py - R[1], 0.0f);
    float fB = !isnan(B) ? B : 0.0f;
    if (fill_h) {
        /* A ratio box whose width is definite or stretches between its insets takes its height from the ratio, not
         * from the block insets (CSS Position 3 section 5.3; aspect-ratio/abspos-003, -005). */
        float rr = 0;
        if (embi_lay_ratio(c, k, &rr) && (!wauto || (!lauto && !rauto))) {
            fill_h = false;
        }
    }
    float fill_bh = 0;
    if (fill_h) {
        float ev = bdr.t + pad.t + bdr.b + pad.b;
        const embi_value *bsv = embi_lay_prop(c, k, EMBI_PROP_box_sizing);
        bool bb = bsv != nullptr && bsv->kind == EMBI_V_KEYWORD && bsv->kw == EMBI_KW_border_box;
        /* min-height and max-height clamp the fill (content-box values gain the edges). */
        float lo = ev, hi = INFINITY;
        const embi_value *mnv = embi_lay_prop(c, k, EMBI_PROP_min_height);
        if (!embi_lay_is_auto(mnv) && !(mnv->kind == EMBI_V_KEYWORD)) {
            float v = embi_lay_len(c, k, mnv, H);
            if (!isnan(v)) {
                lo = bb ? v : v + ev;
            }
        }
        const embi_value *mxv = embi_lay_prop(c, k, EMBI_PROP_max_height);
        bool content_max = mxv != nullptr &&
                           ((mxv->kind == EMBI_V_KEYWORD && (mxv->kw == EMBI_KW_min_content || mxv->kw == EMBI_KW_max_content ||
                                                             mxv->kw == EMBI_KW_fit_content)) ||
                            mxv->kind == EMBI_V_FUNC);
        if (content_max) {
            /* max-height: min-content, max-content or fit-content is the content height (CSS Sizing 3 section 3.2: in the
             * block axis they are the automatic size): lay the box out at its auto height once to know it. */
            embi_lres pr;
            memset(&pr, 0, sizeof pr);
            embi_lay_box(c, k, &cs, &pr);
            if (pr.frag != nullptr) {
                hi = pr.frag->h;
            }
            embi_lay_clear_frags(c, k);
        } else if (mxv != nullptr && !(mxv->kind == EMBI_V_KEYWORD)) {
            float v = embi_lay_len(c, k, mxv, H);
            if (!isnan(v)) {
                hi = bb ? v : v + ev;
            }
        }
        float fill = H - fT - fB - mtN - mbN;
        fill_bh = fminf(fmaxf(fill, lo), hi);
    }
    embi_lres res;
    memset(&res, 0, sizeof res);
    embi_box *save_box = c->force_box;
    float save_h = c->force_h;
    bool save_hs = c->force_hset, save_ws = c->force_wset, save_hn = c->force_hnone;
    if (fill_h && (k->kind == EMBI_BK_FLEX || k->kind == EMBI_BK_GRID || k->kind == EMBI_BK_BLOCK)) {
        c->force_box = k;
        c->force_wset = false;
        c->force_hnone = false;
        c->force_hset = true;
        c->force_h = fill_bh;
    }
    embi_lay_box(c, k, &cs, &res);
    c->force_box = save_box;
    c->force_h = save_h;
    c->force_hset = save_hs;
    c->force_wset = save_ws;
    c->force_hnone = save_hn;
    embi_frag *f = res.frag;
    if (f == nullptr) {
        return;
    }
    /* The subtree gets its relative offsets and stacking flags before it is appended to F. */
    prep_node(c, f);
    if (p->ic_ready) {
        icache_walk(p, f);
    }

    float bw = f->w, bh = f->h;
    if (fill_h) {
        bh = fill_bh;
        f->h = bh;
    }

    /* Horizontal (CSS 2.1 10.3.7, in the direction of the containing block). */
    float mlv = mlN, mrv = mrN, x;
    if (!lauto && !rauto && (mla || mra)) {
        /* Auto margins take the space the insets leave. Two of them share it equally, unless that
         * makes them negative: then the start margin is zero (ltr) or the end margin is (rtl), and
         * the other takes the rest (CSS 2.1 10.3.7). */
        float space = W - L - Rt - bw - mlv - mrv;
        if (mla && mra) {
            if (space >= 0) {
                mlv = mrv = space / 2;
            } else if (crtl) {
                mrv = 0;
                mlv = space;
            } else {
                mlv = 0;
                mrv = space;
            }
        } else if (mla) {
            mlv = space;
        } else {
            mrv = space;
        }
    }
    if (!lauto && !rauto) {
        if (mla || mra) {
            x = R[0] + L + mlv;
        } else {
            /* Both insets set and no auto margin: the box is over-constrained, and the inset on the
             * end that the direction ignores (right in ltr, left in rtl) gives way. justify-self
             * places the box in the inset-modified containing block (CSS Position 3 section 5):
             * start is the start edge of the containing block's direction, which is its right edge
             * in rtl. */
            uint16_t js = align_kw(c, k, EMBI_PROP_justify_self);
            float slack = (W - L - Rt) - bw - mlv - mrv;
            float off = align_offset(js, slack);
            x = crtl ? R[2] - Rt - mrv - bw - off : R[0] + L + mlv + off;
        }
    } else if (!lauto) {
        x = R[0] + L + mlv;
    } else if (!rauto) {
        x = R[2] - Rt - mrv - bw;
    } else {
        /* Both insets auto: a center or end alignment places the box in the containing block (CSS
         * Position 3 section 5.2); otherwise it sits at its static position. */
        uint16_t js = align_kw(c, k, EMBI_PROP_justify_self);
        if (r->has_cb && r->sbox != nullptr && (js == 0 || js == EMBI_KW_auto)) {
            js = align_kw(c, r->sbox, EMBI_PROP_justify_items); /* auto takes the grid container's justify-items */
        }
        float free = W - bw - mlv - mrv;
        if (r->flex) {
            /* A flex child: the sole-item position in the container's content box (W3e). */
            bool fl = r->fflip[0];
            x = ox + flex_axis(r, 0, bw, fl ? mrv : mlv, fl ? mlv : mrv);
            /* Default overflow alignment (CSS Position 3 section 5.3): when the flex container is not the containing
             * block, a box that overflows but fits in its containing block is shifted to stay inside it
             * when it is centered or end-aligned (position-absolute-center-001). A flex container that is the containing block keeps the unsafe
             * position (flex-abspos-staticpos-*). */
            if (sf != F && r->ffrac[0] > 0.0f && bw + mlv + mrv <= R[2] - R[0]) {
                x = fminf(fmaxf(x, R[0] + mlv), R[2] - mrv - bw);
            }
        } else if (js == EMBI_KW_center) {
            x = R[0] + mlv + free / 2;
        } else if (js == EMBI_KW_right) {
            x = R[0] + mlv + free;
        } else if (js == EMBI_KW_left) {
            x = R[0] + mlv;
        } else if (is_end_align(js)) {
            x = crtl ? R[0] + mlv : R[0] + mlv + free;
        } else if (r->has_cb) {
            /* A grid area is the containing block: the start edge of its direction (CSS Grid 2 section 9). */
            x = crtl ? R[2] - mrv - bw : R[0] + mlv;
        } else {
            x = srtl ? px - mrv - bw : px + mlv;
        }
    }

    /* Vertical (CSS 2.1 10.6.4): over-constrained boxes ignore the bottom inset. */
    float mtv = mtN, mbv = mbN, y;
    if (!tauto && !bauto) {
        float space = H - T - B - bh - mtv - mbv;
        if (mta && mba) {
            /* Equal margins, even when negative (CSS 2.1 10.6.4). */
            mtv = mbv = space / 2;
        } else if (mta) {
            mtv = space;
        } else if (mba) {
            mbv = space;
        }
        float off = 0;
        if (!mta && !mba) {
            /* Both insets set: align-self places the box in the inset-modified containing block. */
            off = align_offset(align_kw(c, k, EMBI_PROP_align_self), space);
        }
        y = R[1] + T + mtv + off;
    } else if (!tauto) {
        y = R[1] + T + mtv;
    } else if (!bauto) {
        y = R[3] - B - mbv - bh;
    } else {
        uint16_t as = align_kw(c, k, EMBI_PROP_align_self);
        if (r->has_cb && r->sbox != nullptr && (as == 0 || as == EMBI_KW_auto)) {
            as = align_kw(c, r->sbox, EMBI_PROP_align_items); /* auto takes the grid container's align-items */
        }
        float free = H - bh - mtv - mbv;
        if (r->flex) {
            bool fl = r->fflip[1];
            y = oy + flex_axis(r, 1, bh, fl ? mbv : mtv, fl ? mtv : mbv);
            if (sf != F && r->ffrac[1] > 0.0f && bh + mtv + mbv <= R[3] - R[1]) {
                y = fminf(fmaxf(y, R[1] + mtv), R[3] - mbv - bh);
            }
        } else if (as == EMBI_KW_center) {
            y = R[1] + mtv + free / 2;
        } else if (is_end_align(as)) {
            y = R[1] + mtv + free;
        } else if (r->has_cb) {
            y = R[1] + mtv;
        } else {
            y = py + mtv;
        }
    }

    f->x = x;
    f->y = y;
    embi_lay_frag_set_mg(c->ld, f, mlv, mtv, mrv, mbv);
    embi_lay_frag_append(F, f);
    embi_lay_ov_add_child(F, f);
    for (embi_frag *q = F; q->parent != nullptr; q = q->parent) {
        embi_lay_ov_add_child(q->parent, q);
    }
}

void embi_lay_position_finish(embi_lctx *c, embi_frag *icb) {
    struct embi_pos *p = c->pos;
    c->ld->lc_on = false; /* the fragments change in place from here on: nothing is given back from the layout cache */
    /* The flow's relative offsets and stacking flags come first: the static positions and
     * containing blocks that the placement reads are then in their final frames. */
    prep_node(c, icb);
    if (p != nullptr) {
        /* The records are placed in order. A record whose box has a later record is a layout that
         * was replaced: it is skipped (the map is kept current as records are pushed, so this also
         * holds for the records that a placed box's layout pushes). */
        for (uint32_t i = 0; i < p->recs.len; i++) {
            embi_pos_rec r = p->recs.data[i];
            uint64_t j = 0;
            if (embi_hashmap_get(&p->last, (uint64_t)(uintptr_t)r.box, &j) && j != i) {
                continue;
            }
            place_one(c, &r, icb);
        }
    }
    if (c->ld->has_sticky) {
        walk_sticky(c, icb, icb);
    }
    if (p != nullptr) {
        embi_hashmap_free(&p->last);
        embi_hashmap_free(&p->icmap);
        embi_vec_free(&p->recs);
        embi_vec_free(&p->ic);
        embi_free(p, sizeof *p);
        c->pos = nullptr;
    }
}
