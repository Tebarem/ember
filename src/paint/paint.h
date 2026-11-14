#pragma once
/*
 * The display list (W4c). It is built from the fragment tree of a laid-out
 * document in CSS 2.1 Appendix E paint order, and it is a plain CPU structure: nothing here touches the
 * GPU. W4b's renderer replays it (embi_frame_* calls), and W4d compares two lists item by item.
 *
 * Layout of the list. Items are kept in one array in paint order. A stacking context is a contiguous
 * range of that array (embi_dctx.first .. end), and a context's nested contexts lie inside its range, so
 * a dirty context can be rebuilt alone (W6). Every item has a bounds rect in page px (the untransformed
 * coordinates of the layout), a transform index into xforms (the composed 2D transforms of its ancestors,
 * applied to the rect by the renderer, 0 is the identity) and a clip index into clips (a chain through
 * the parent field, 0 is no clip). Opacity below 1 is either folded into the colors of one item or kept
 * as a LAYER_BEGIN .. LAYER_END pair.
 *
 * Lifetime. The text items and the image URLs refer to the layout of the document: the runs are the
 * layout's shaped runs, valid until the document is laid out again, so a list is valid for one build
 * (the same rule as the fragment tree). embi_dl_reset keeps the arrays, so a pooled list does not
 * allocate on every frame.
 */
#include "paint/dl.h"
#include "layout/layout.h"
#include "svg/svg.h"

#include <stdbool.h>
#include <stdint.h>

struct embi_document;

/* ---- W5e: the state of a form control as the painter needs it (src/paint/control.c) ---- */
enum {
    EMBI_PCTL_NONE = 0,
    EMBI_PCTL_TEXT,     /* single-line text field (text, search, email, url, tel, password, number) */
    EMBI_PCTL_TEXTAREA, /* multi-line text, wrapped unless nowrap */
    EMBI_PCTL_BUTTON,   /* button, input button / submit / reset: the element's own box holds the label */
    EMBI_PCTL_CHECKBOX,
    EMBI_PCTL_RADIO,
    EMBI_PCTL_SELECT,   /* drop-down: the selected option's label and an arrow; open draws the popup list */
    EMBI_PCTL_LISTBOX,  /* select multiple / size > 1: the options in flow */
    EMBI_PCTL_RANGE,
    EMBI_PCTL_PROGRESS,
    EMBI_PCTL_METER,
};

/* One option of a select (label is UTF-8 and stays valid until the next state call for any control). */
typedef struct embi_paint_opt {
    const char *label;
    uint32_t label_len;
    bool selected, disabled;
} embi_paint_opt;

/* The row height of the options of a select list and of its popup, in CSS px. */
#define EMBI_CTL_ROW_H 24.0f

typedef struct embi_paint_ctl {
    uint8_t kind;                  /* EMBI_PCTL_* */
    bool focused, disabled, readonly;
    bool checked, indeterminate;   /* checkbox and radio */
    bool caret_on;                 /* the blink phase: the caret is drawn */
    bool password;                 /* the text is drawn as bullets */
    bool nowrap;                   /* textarea wrap=off */
    bool open;                     /* select: the popup list is open */
    const char *text;              /* the value (UTF-8); for a select the label of the selected option */
    uint32_t text_len;
    const char *placeholder;       /* drawn while text_len is 0 */
    uint32_t placeholder_len;
    uint32_t sel_a, sel_b;         /* selection in bytes of text, sel_a <= sel_b */
    uint32_t caret;                /* the caret (selection focus) in bytes of text */
    uint32_t comp_a, comp_b;       /* the composition range in bytes (underlined) when comp_on */
    bool comp_on;
    double value, min, max;        /* range, progress, meter */
    double low, high, optimum;     /* meter */
    bool indeterminate_bar;        /* progress without a value */
    const embi_paint_opt *opts;    /* select: the options, nopts of them (nullptr for none) */
    uint32_t nopts;
    int32_t hot;                   /* select popup: the highlighted option index (-1 none) */
    uint32_t first_row;            /* listbox: the first visible option */
} embi_paint_ctl;

/* Fills the state of the control el. False when the element has no state (the painter then reads the attributes). The hook
 * lives on the layout engine (embi_layout_set_control_hook), whose out is an embi_paint_ctl. */
typedef bool (*embi_paint_control_fn)(void *user, const embi_element *el, embi_paint_ctl *out);

/* The configuration of a build. image_size gives the natural size of a URL (false when unknown); it is
 * used for backgrounds with auto sizes and for replaced images. focus_rgba is the color of outline-style:
 * auto. A zero config is valid. */
typedef struct embi_paint_cfg {
    void *user;
    embi_layout_image_size_fn image_size;
    uint32_t focus_rgba;
    float viewport_w, viewport_h; /* the viewport in CSS px (the canvas and vw/vh units); 0 takes the root's size */
    float scale;                  /* device pixels per CSS px: box edges snap to device pixels (0 takes 1) */
} embi_paint_cfg;

/* W5e (control.c): the byte offset in the value of a text control that is at the page point (x, y), for placing the caret
 * from the pointer. bb is the control's border box in page px (embi_layout_border_box). False when el is not a laid out text
 * control. The state comes from the engine's control hook (the attributes when it has none). */
bool embi_paint_control_offset(const embi_layout_engine *e, const embi_element *el, const float bb[4], float x, float y,
                               uint32_t *byte);
/* The caret rectangle (x, y, w, h) in page px of a text control (the IME area and the blink damage). */
bool embi_paint_control_caret(const embi_layout_engine *e, const embi_element *el, const float bb[4], float rect[4]);

/* The track of a range input whose parts are styled (appearance: none with a ::-webkit-slider-runnable-track or
 * ::-webkit-slider-thumb style): *left and *width are the content box of the track along the page's x axis for the input's
 * border box bb (page px) and *thumb_w the width of the thumb, so that the thumb's left edge runs from *left to *left + *width -
 * *thumb_w with the value. False for a range drawn natively. */
bool embi_paint_range_track(const embi_layout_engine *e, embi_style_engine *se, const embi_element *el, const float bb[4],
                            float *left, float *width, float *thumb_w);

/* R4c: the host's lookup of SVG images by URL as written in the document (called with the build's cfg->user). It returns the
 * parsed SVG image behind the URL, or nullptr when the URL is not one (or not loaded). Process-wide: set it once; nullptr
 * (the default) means no SVG images, and <img src=x.svg> then paints nothing. */
void embi_paint_set_svg_image_fn(const embi_svg_image *(*fn)(void *user, embi_str url));

/* Builds the display list of the document's last layout (through the engine e) into d, which is reset first. A
 * document with no layout gives an empty list. Allocation failure is fatal: the allocator aborts (embi_alloc), so
 * the build has no failure result. */
void embi_paint_document(embi_dl *d, const embi_layout_engine *e, const embi_document *doc,
                         const embi_paint_cfg *cfg);

/* ---- W6b (incr.c): the incremental display list ---- */

/* A cache of the last list of a document. embi_paint_document_incr builds the list like embi_paint_document does, but when
 * allow is true and the layout reports that only the paint of some elements changed since the last call
 * (embi_layout_paint_dirty), only the stacking contexts that hold them are built again and spliced into the cached list.
 * Everything else, and every case this cannot do exactly, is a full build. The list is copied into out (reset first). */
typedef struct embi_paint_cache embi_paint_cache;
embi_paint_cache *embi_paint_cache_create(void);
void embi_paint_cache_free(embi_paint_cache *c);
void embi_paint_document_incr(embi_paint_cache *c, embi_dl *out, embi_layout_engine *e, const embi_document *doc,
                              const embi_paint_cfg *cfg, bool allow);
/* Of the last call: the number of stacking contexts built again (0 for a full build), and whether it was incremental. */
/* scroll60: a paint after a change of scroll offsets only. When the cache's last paint was of this document at this layout and
 * nothing but the offsets of the scroll containers differs (same fragments, nothing dirty, same config), the list that is on
 * screen (list: its first content_len items are the page, the rest its scrollbars) is moved to the new offsets in place:
 * its content is translated, nothing is built, and it is cut back to the page. Returns 0 when it cannot (paint as usual, the
 * list is not touched), 1 when no offset changed (the list is not touched), 2 when it was moved; dmg then holds what moved. */
int embi_paint_document_scroll(embi_paint_cache *c, embi_dl *list, uint32_t content_len, embi_layout_engine *e,
                               const embi_document *doc, const embi_paint_cfg *cfg, embi_scroll_damage *dmg);
uint32_t embi_paint_cache_rebuilt(const embi_paint_cache *c);
bool embi_paint_cache_last_incremental(const embi_paint_cache *c);
/* The number of incremental and of full builds so far. */
uint64_t embi_paint_cache_count(const embi_paint_cache *c, bool incremental);
/* Whether two lists draw the same: the items, the contexts and the clips and transforms they name, compared by content. */
bool embi_dl_equal(const embi_dl *a, const embi_dl *b);
