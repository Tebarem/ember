#pragma once
/*
 * Scrolling of W4d. Scroll offsets live on the scroll container fragments (W3h, the side
 * record embi_fext); the display list build reads them (paint.c enter() subtracts them), so a scroll changes the list
 * and the damage between two lists is the scroll's repaint. Nothing here draws.
 *
 *  - embi_paint_scroll_by clamps a scroll to the scrollable overflow of its container (CSS Overflow 3: the overflow
 *    rectangle already includes the padding end, see block.c) and stores it through the layout engine. The root scroller
 *    scrolls the viewport: its range comes from the viewport size, not from its own box.
 *  - sticky children move with their container's offset (embi_scroll_sticky_offset is W3's sticky constraint).
 *  - programmatic scrolls with scroll-behavior: smooth animate over EMBI_SCROLL_SMOOTH_MS with an ease-out curve;
 *    wheel and keyboard input is immediate (the caller does not use the animation for them).
 *  - overlay scrollbars are drawn while a container scrolls and fade out EMBI_SB_VISIBLE_MS after the last scroll.
 */
#include "layout/fragment.h"
#include "layout/layout.h"
#include "paint/dl.h"

#include <stdbool.h>
#include <stdint.h>

struct embi_style;
struct embi_style_engine;
struct embi_element;

#define EMBI_SCROLL_SMOOTH_MS 150.0
#define EMBI_SB_VISIBLE_MS 1000.0 /* a scrollbar stays opaque this long after the last scroll */
#define EMBI_SB_FADE_MS 200.0     /* then it fades out over this long */
#define EMBI_SB_MIN_THUMB 16.0f   /* the thumb's minimum length in px */

/* The maximum scroll offsets of a scroll container box f (its own size and overflow). The offset range starts at 0. */
void embi_scroll_max(const embi_frag *f, float *max_x, float *max_y);
/* The maximum offsets of the root scroller over a viewport of vw x vh CSS px, from the root's overflow rectangle. */
void embi_scroll_max_root(const embi_frag *root, float vw, float vh, float *max_x, float *max_y);

/* Scrolls the container f by (dx, dy) CSS px, clamped to its range, and stores the result through the engine. root:
 * f is the root scroller (its range is the viewport of vw x vh). Returns true when the offset changed. */
bool embi_paint_scroll_by(embi_layout_engine *e, const embi_document *doc, embi_frag *f, float dx, float dy, bool root,
                          float vw, float vh);

/* A scroll offset rounded to a whole device px at scale device px per CSS px: the offset a list is built for. */
float embi_scroll_quantize(float off, float scale);

/* The offset a sticky fragment takes at the current scroll of its container (W3's constraints): dx, dy in px. */
void embi_scroll_sticky_offset(const embi_frag *f, float *dx, float *dy);

/* scroll-behavior: smooth on the style of the scrolling element. */
bool embi_scroll_is_smooth(const struct embi_style *s);

/* An animated programmatic scroll (scroll-behavior: smooth). The times are in ms of any monotonic clock. */
typedef struct embi_scroll_anim {
    float x0, y0, x1, y1; /* from, to */
    double t0;            /* start time */
    bool active;
} embi_scroll_anim;

/* Starts an animation from (x0, y0) to (x1, y1) at now_ms (it replaces any running one). */
void embi_scroll_anim_start(embi_scroll_anim *a, float x0, float y0, float x1, float y1, double now_ms);
/* The offset at now_ms into x, y. Returns true while the animation runs; at its end the target is written and the
 * animation stops. */
bool embi_scroll_anim_sample(embi_scroll_anim *a, double now_ms, float *x, float *y);
/* The ease-out curve of the smooth scroll (cubic, 0 at 0, 1 at 1). */
float embi_scroll_ease_out(float t);

/* The overlay scrollbar fade of one container: the time of its last scroll. */
typedef struct embi_sb_fade {
    double last_ms;
    bool seen;
} embi_sb_fade;

void embi_sb_note_scroll(embi_sb_fade *f, double now_ms);
/* The opacity of the scrollbar at now_ms: 0 before any scroll, 1 for EMBI_SB_VISIBLE_MS after the last one, then a
 * linear fade to 0 over EMBI_SB_FADE_MS. */
float embi_sb_alpha(const embi_sb_fade *f, double now_ms);

/* The colors and width of a scrollbar: RGBA8 (r in the low byte) of the thumb and the track, and the width in px. */
typedef struct embi_sb_style {
    float width;
    uint32_t thumb, track;
    bool vbar_left; /* the vertical bar sits on the left edge (direction: rtl in a horizontal writing mode) */
} embi_sb_style;

/* The defaults of an overlay scrollbar (scrollbar-width: auto, scrollbar-color: auto). */
embi_sb_style embi_sb_style_default(void);

/* scrollbar-width as the CSS module computes it: auto (8 px), thin (4 px) or none (no bar). */
enum embi_sb_width {
    EMBI_SBW_AUTO = 0,
    EMBI_SBW_THIN,
    EMBI_SBW_NONE,
};
/* The style of a scrollbar from its computed scrollbar-width and scrollbar-color. has_colors is false for
 * scrollbar-color: auto (the defaults of embi_sb_style_default); otherwise thumb and track are the RGBA8 colors of
 * the value (r in the low byte). */
embi_sb_style embi_sb_style_resolve(enum embi_sb_width width, bool has_colors, uint32_t thumb, uint32_t track);
/* The style of the scrollbars of an element from its computed style: scrollbar-width (auto, thin, none), scrollbar-color
 * (auto, or the thumb and track colors, currentcolor and light-dark() resolved) and, for the side of the vertical bar,
 * direction and writing-mode. A null style gives the defaults. */
embi_sb_style embi_sb_style_from(const struct embi_style *s);

/* Appends the thumb and the track of the scroll container f (a box fragment whose border box starts at (x, y) in canvas
 * px) to d: the vertical bar on the right edge of the padding box (the left one with st->vbar_left) when f overflows vertically,
 * the horizontal bar on the bottom edge when it overflows horizontally; when both show, each track stops short of the
 * other bar, so they do not overlap in the corner. Each at the scroll offset of f (embi_frag_sx/sy). alpha (embi_sb_alpha at the
 * current time) scales both colors; nothing is appended for alpha <= 0, a width of 0 or no overflow. The items are FILL
 * items with no transform and no clip, in canvas space. The caller appends them after the container's content so they
 * paint on top (W4e does this for the scrollers that scrolled recently). Returns the number of items appended. */
uint32_t embi_sb_emit(embi_dl *d, const embi_frag *f, float x, float y, const embi_sb_style *st, float alpha);

/* The thumb of a scrollbar along a track of track px for a client of client px over content of content px at offset
 * offset. start and len are in px along the track. Returns false when there is nothing to scroll (no bar). */
bool embi_sb_thumb(float track, float client, float content, float offset, float *start, float *len);

/* ------------------------------------------------------------------ styled (classic) scrollbars
 *
 * A scroll container with a ::-webkit-scrollbar style has classic scrollbars: they take their thickness from the container
 * (the layout reserves it as the gutter), they show whenever the content overflows, and their parts are painted from the
 * styles of ::-webkit-scrollbar, ::-webkit-scrollbar-track, ::-webkit-scrollbar-thumb and ::-webkit-scrollbar-corner. A style
 * with display: none or no thickness removes the bars. */

typedef enum embi_sb_pe_kind {
    EMBI_SBPE_NONE = 0, /* no ::-webkit-scrollbar style: the overlay scrollbars of embi_sb_emit */
    EMBI_SBPE_STYLED,
    EMBI_SBPE_HIDDEN,   /* the container shows no scrollbar */
} embi_sb_pe_kind;

typedef struct embi_sb_pe {
    float vert, horz; /* the width of the vertical bar and the height of the horizontal one */
    bool vbar_left;
    const struct embi_style *bar, *thumb, *track, *corner; /* nullptr: nothing is painted for the part */
    const struct embi_style *thumb_hover;                   /* the thumb as the hovered bar shows it (owned), or nullptr */
    int hover_axis;                                         /* 1: the pointer is over the vertical bar's thumb, 2: the horizontal */
} embi_sb_pe;

/* The scrollbar parts of el (the element is the key of its scroller). hover_axis: 1 or 2 when the pointer is over the thumb of
 * the vertical or horizontal bar (the thumb then takes its :hover style); the caller releases the result with embi_sb_pe_free. */
embi_sb_pe_kind embi_sb_pe_from(struct embi_style_engine *se, struct embi_element *el, int hover_axis, embi_sb_pe *out);
void embi_sb_pe_free(struct embi_style_engine *se, embi_sb_pe *pe);

/* Where the bars of the scroll container f (its border box starts at (x, y)) are: each bar's track (x, y, w, h), its thumb, the
 * corner, and along the axis the thumb's start and length and the track's length. */
typedef struct embi_sb_geo {
    bool vert, horz;
    float v[4], h[4];       /* the vertical and the horizontal bar */
    float vthumb[4], hthumb[4];
    float corner[4];
    bool has_corner;
    float vstart, vlen, hstart, hlen; /* the thumbs along their tracks */
    float vtrack, htrack;             /* the tracks' lengths */
    float vmax, hmax;                 /* the scroll ranges */
} embi_sb_geo;
bool embi_sb_geometry(const embi_frag *f, float x, float y, const embi_sb_pe *pe, embi_sb_geo *g);

/* Appends the bars of f to d: for each its own box, the track, the thumb (the hovered one with the hovered style), and the
 * corner. clip is the rectangle (x0, y0, x1, y1) that the bars are cut by. Returns the number of items appended. */
uint32_t embi_sb_emit_styled(embi_dl *d, const embi_frag *f, float x, float y, const embi_sb_pe *pe, const float clip[4]);
