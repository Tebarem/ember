/*
 * Serialization of the compound value kinds (W2c): images and gradients, positions, transforms,
 * shadows, filters, basic shapes, easing functions, grid tracks and lines, ratios and functional
 * values. The text is canonical CSS (lowercase function names, single spaces, ", " between
 * arguments) and parses back to the same structure.
 */
#include "css/value_int.h"

static void put(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static void put_num(embi_sb *sb, double v)
{
    embi_css_append_num(sb, v);
}

static void put_val(embi_sb *sb, const embi_value *v)
{
    embi_value_serialize(sb, v);
}

static void put_str(embi_sb *sb, embi_str s)
{
    embi_sb_append_char(sb, '"');
    uint32_t run = 0; /* CF1: the characters that need no escape are appended a run at a time */
    for (uint32_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == '"' || s.ptr[i] == (char)0x5c) {
            embi_sb_append(sb, (embi_str){s.ptr + run, i - run});
            embi_sb_append_char(sb, (char)0x5c);
            run = i;
        }
    }
    embi_sb_append(sb, (embi_str){s.ptr + run, s.len - run});
    embi_sb_append_char(sb, '"');
}

/* ------------------------------------------------------------------ positions */

static void put_axis(embi_sb *sb, const embi_pos_axis *ax, bool horizontal)
{
    if (ax->anchor == 1) {
        put(sb, horizontal ? "right " : "bottom ");
    }
    put_val(sb, &ax->off);
}

static void put_position(embi_sb *sb, const embi_position *p)
{
    put_axis(sb, &p->x, true);
    embi_sb_append_char(sb, ' ');
    put_axis(sb, &p->y, false);
    if (p->z.kind != EMBI_V_NONE) {
        embi_sb_append_char(sb, ' ');
        put_val(sb, &p->z);
    }
}

/* ------------------------------------------------------------------ images */

static const char *interp_name(uint8_t s)
{
    static const char *const names[] = {
        "", "srgb", "srgb-linear", "display-p3", "a98-rgb", "prophoto-rgb", "rec2020", "lab",
        "oklab", "xyz-d50", "xyz", "hsl", "hwb", "lch", "oklch", "display-p3-linear",
    };
    return s < sizeof names / sizeof names[0] ? names[s] : "";
}

static void put_image(embi_sb *sb, const embi_image *img);

static void put_stops(embi_sb *sb, const embi_gradient *g, bool lead_comma)
{
    for (uint32_t i = 0; i < g->nstops; i++) {
        const embi_color_stop *s = &g->stops[i];
        if (i > 0 || lead_comma) {
            put(sb, ", ");
        }
        if (s->color.kind != EMBI_V_NONE) {
            put_val(sb, &s->color);
            for (int k = 0; k < 2; k++) {
                if (s->pos[k].kind != EMBI_V_NONE) {
                    embi_sb_append_char(sb, ' ');
                    put_val(sb, &s->pos[k]);
                }
            }
        } else {
            put_val(sb, &s->pos[0]);
        }
    }
}

static void put_interp(embi_sb *sb, const embi_gradient *g, bool *first)
{
    if (!g->has_interp) {
        return;
    }
    if (!*first) {
        embi_sb_append_char(sb, ' ');
    }
    *first = false;
    put(sb, "in ");
    put(sb, interp_name(g->interp));
    if (g->hue != EMBI_HUE_SHORTER) {
        put(sb, g->hue == EMBI_HUE_LONGER ? " longer hue" : g->hue == EMBI_HUE_INCREASING ? " increasing hue" : " decreasing hue");
    }
}

static void put_gradient(embi_sb *sb, const embi_gradient *g)
{
    static const char *const names[] = {"", "linear", "radial", "conic"};
    if (g->repeating) {
        put(sb, "repeating-");
    }
    put(sb, names[g->kind]);
    put(sb, "-gradient(");
    bool first = true;
    if (g->kind == EMBI_GRAD_LINEAR) {
        if (g->sides) {
            put(sb, "to");
            if (g->sides & EMBI_SIDE_TOP) put(sb, " top");
            if (g->sides & EMBI_SIDE_BOTTOM) put(sb, " bottom");
            if (g->sides & EMBI_SIDE_LEFT) put(sb, " left");
            if (g->sides & EMBI_SIDE_RIGHT) put(sb, " right");
            first = false;
        } else if (g->angle.kind != EMBI_V_NONE) {
            put_val(sb, &g->angle);
            first = false;
        }
        put_interp(sb, g, &first);
    } else if (g->kind == EMBI_GRAD_RADIAL) {
        put(sb, g->ellipse ? "ellipse" : "circle");
        first = false;
        if (g->size == EMBI_RS_EXPLICIT) {
            embi_sb_append_char(sb, ' ');
            put_val(sb, &g->radius[0]);
            if (g->ellipse) {
                embi_sb_append_char(sb, ' ');
                put_val(sb, &g->radius[1]);
            }
        } else if (g->size != EMBI_RS_FARTHEST_CORNER) {
            put(sb, g->size == EMBI_RS_CLOSEST_SIDE ? " closest-side"
                    : g->size == EMBI_RS_FARTHEST_SIDE ? " farthest-side" : " closest-corner");
        }
        if (g->has_pos) {
            put(sb, " at ");
            put_position(sb, &g->pos);
        }
        put_interp(sb, g, &first);
    } else {
        if (g->angle.kind != EMBI_V_NONE) {
            put(sb, "from ");
            put_val(sb, &g->angle);
            first = false;
        }
        if (g->has_pos) {
            put(sb, first ? "at " : " at ");
            put_position(sb, &g->pos);
            first = false;
        }
        put_interp(sb, g, &first);
    }
    put_stops(sb, g, !first);
    embi_sb_append_char(sb, ')');
}

static void put_image(embi_sb *sb, const embi_image *img)
{
    switch (img->kind) {
    case EMBI_IMG_URL:
        put(sb, "url(");
        put_str(sb, img->url);
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_IMG_GRADIENT:
        put_gradient(sb, img->grad);
        break;
    case EMBI_IMG_IMAGE_SET:
        put(sb, "image-set(");
        for (uint32_t i = 0; i < img->n; i++) {
            if (i) {
                put(sb, ", ");
            }
            put_image(sb, img->opts[i].img);
            if (img->opts[i].dppx != 1.0) {
                embi_sb_append_char(sb, ' ');
                put_num(sb, img->opts[i].dppx);
                put(sb, "x");
            }
            if (img->opts[i].type.len) {
                put(sb, " type(");
                put_str(sb, img->opts[i].type);
                embi_sb_append_char(sb, ')');
            }
        }
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_IMG_CROSS_FADE:
        put(sb, "cross-fade(");
        for (uint32_t i = 0; i < img->n; i++) {
            if (i) {
                put(sb, ", ");
            }
            if (img->xf[i].pct >= 0) {
                put_num(sb, img->xf[i].pct);
                put(sb, "% ");
            }
            if (img->xf[i].img) {
                put_image(sb, img->xf[i].img);
            } else {
                put_val(sb, &img->xf[i].color);
            }
        }
        embi_sb_append_char(sb, ')');
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ transforms, filters, shadows */

static const char *xf_name(uint8_t fn)
{
    static const char *const names[] = {
        "", "matrix", "matrix3d", "translate", "translateX", "translateY", "translateZ",
        "translate3d", "scale", "scaleX", "scaleY", "scaleZ", "scale3d", "rotate", "rotateX",
        "rotateY", "rotateZ", "rotate3d", "skew", "skewX", "skewY", "perspective",
    };
    return fn < sizeof names / sizeof names[0] ? names[fn] : "";
}

static void put_shadow(embi_sb *sb, const embi_shadow *s, bool drop)
{
    if (s->inset) {
        put(sb, "inset ");
    }
    put_val(sb, &s->x);
    embi_sb_append_char(sb, ' ');
    put_val(sb, &s->y);
    bool blur0 = s->blur.kind == EMBI_V_LENGTH && s->blur.num == 0;
    bool spread0 = s->spread.kind == EMBI_V_LENGTH && s->spread.num == 0;
    (void)drop;
    if (!blur0 || !spread0) {
        embi_sb_append_char(sb, ' ');
        put_val(sb, &s->blur);
    }
    if (!spread0) {
        embi_sb_append_char(sb, ' ');
        put_val(sb, &s->spread);
    }
    if (!(s->color.kind == EMBI_V_COLOR && s->color.color.space == EMBI_CS_CURRENT)) {
        embi_sb_append_char(sb, ' ');
        put_val(sb, &s->color);
    }
}

static void put_filter(embi_sb *sb, const embi_filter *f)
{
    static const char *const names[] = {
        "", "blur", "brightness", "contrast", "grayscale", "hue-rotate", "invert", "opacity",
        "saturate", "sepia", "drop-shadow", "url",
    };
    if (f->fn == EMBI_FILTER_URL) {
        put_val(sb, &f->arg);
        return;
    }
    put(sb, names[f->fn]);
    embi_sb_append_char(sb, '(');
    if (f->fn == EMBI_FILTER_DROP_SHADOW) {
        put_shadow(sb, f->shadow, true);
    } else {
        put_val(sb, &f->arg);
    }
    embi_sb_append_char(sb, ')');
}

/* ------------------------------------------------------------------ shapes, easing, grid */

static void put_radii(embi_sb *sb, const embi_shape *s)
{
    if (!s->has_round) {
        return;
    }
    put(sb, " round");
    for (int i = 0; i < 4; i++) {
        embi_sb_append_char(sb, ' ');
        put_val(sb, &s->radii[i]);
    }
    bool same = true;
    for (int i = 0; i < 4; i++) {
        same = same && s->radii[i].kind == s->radii[4 + i].kind && s->radii[i].num == s->radii[4 + i].num &&
               s->radii[i].unit == s->radii[4 + i].unit;
    }
    if (!same) {
        put(sb, " /");
        for (int i = 4; i < 8; i++) {
            embi_sb_append_char(sb, ' ');
            put_val(sb, &s->radii[i]);
        }
    }
}

static void put_shape(embi_sb *sb, const embi_shape *s)
{
    switch (s->kind) {
    case EMBI_SHAPE_INSET:
    case EMBI_SHAPE_RECT:
        put(sb, s->kind == EMBI_SHAPE_INSET ? "inset(" : "rect(");
        for (int i = 0; i < 4; i++) {
            if (i) {
                embi_sb_append_char(sb, ' ');
            }
            put_val(sb, &s->box[i]);
        }
        put_radii(sb, s);
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_SHAPE_XYWH:
        put(sb, "xywh(");
        for (int i = 0; i < 4; i++) {
            if (i) {
                embi_sb_append_char(sb, ' ');
            }
            put_val(sb, &s->box[i]);
        }
        put_radii(sb, s);
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_SHAPE_CIRCLE:
    case EMBI_SHAPE_ELLIPSE:
        put(sb, s->kind == EMBI_SHAPE_CIRCLE ? "circle(" : "ellipse(");
        put_val(sb, &s->r[0]);
        if (s->kind == EMBI_SHAPE_ELLIPSE) {
            embi_sb_append_char(sb, ' ');
            put_val(sb, &s->r[1]);
        }
        if (s->has_at) {
            put(sb, " at ");
            put_position(sb, &s->at);
        }
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_SHAPE_POLYGON:
        put(sb, "polygon(");
        if (s->evenodd) {
            put(sb, "evenodd, ");
        }
        for (uint32_t i = 0; i < s->npts; i++) {
            if (i) {
                put(sb, ", ");
            }
            put_val(sb, &s->pts[i * 2]);
            embi_sb_append_char(sb, ' ');
            put_val(sb, &s->pts[i * 2 + 1]);
        }
        embi_sb_append_char(sb, ')');
        break;
    case EMBI_SHAPE_SHAPE: {
        put(sb, "shape(");
        if (s->evenodd) {
            put(sb, "evenodd ");
        }
        put(sb, "from ");
        put_position(sb, &s->from);
        for (uint32_t i = 0; i < s->ncmds; i++) {
            const embi_shape_cmd *c = &s->cmds[i];
            static const char *const names[] = {"", "move", "line", "hline", "vline", "curve", "smooth", "arc", "close"};
            put(sb, ", ");
            put(sb, names[c->op]);
            if (c->op == EMBI_SOP_CLOSE) {
                continue;
            }
            put(sb, c->rel ? " by " : " to ");
            if (c->op == EMBI_SOP_HLINE || c->op == EMBI_SOP_VLINE) {
                put_axis(sb, &c->p[0], c->op == EMBI_SOP_HLINE);
                continue;
            }
            put_axis(sb, &c->p[0], true);
            embi_sb_append_char(sb, ' ');
            put_axis(sb, &c->p[1], false);
            for (uint32_t k = 0; k < c->ncp; k++) {
                put(sb, k == 0 ? " with " : " / ");
                put_axis(sb, &c->c[k][0], true);
                embi_sb_append_char(sb, ' ');
                put_axis(sb, &c->c[k][1], false);
            }
            if (c->op == EMBI_SOP_ARC) {
                put(sb, " of ");
                put_val(sb, &c->r[0]);
                if (c->nr > 1) {
                    embi_sb_append_char(sb, ' ');
                    put_val(sb, &c->r[1]);
                }
                put(sb, c->cw ? " cw" : " ccw");
                put(sb, c->large ? " large" : " small");
                if (c->rot != 0.0) {
                    put(sb, " rotate ");
                    put_num(sb, c->rot);
                    put(sb, "deg");
                }
            }
        }
        embi_sb_append_char(sb, ')');
        break;
    }
    case EMBI_SHAPE_PATH:
        put(sb, "path(");
        if (s->evenodd) {
            put(sb, "evenodd, ");
        }
        put_str(sb, s->path);
        embi_sb_append_char(sb, ')');
        break;
    default:
        break;
    }
}

static void put_easing(embi_sb *sb, const embi_easing *e)
{
    if (e->kind == EMBI_EASE_CUBIC) {
        put(sb, "cubic-bezier(");
        for (int i = 0; i < 4; i++) {
            if (i) {
                put(sb, ", ");
            }
            put_num(sb, e->c[i]);
        }
        embi_sb_append_char(sb, ')');
    } else if (e->kind == EMBI_EASE_STEPS) {
        put(sb, "steps(");
        put_num(sb, e->steps);
        static const char *const j[] = {"jump-end", "jump-start", "jump-none", "jump-both"};
        put(sb, ", ");
        put(sb, j[e->jump]);
        embi_sb_append_char(sb, ')');
    } else {
        put(sb, "linear(");
        for (uint32_t i = 0; i < e->npts; i++) {
            if (i) {
                put(sb, ", ");
            }
            put_num(sb, e->pts[i].out);
            embi_sb_append_char(sb, ' ');
            put_num(sb, e->pts[i].in * 100.0);
            embi_sb_append_char(sb, '%');
        }
        embi_sb_append_char(sb, ')');
    }
}

static void put_breadth(embi_sb *sb, const embi_track_breadth *b)
{
    switch (b->kind) {
    case EMBI_TB_AUTO:
        put(sb, "auto");
        break;
    case EMBI_TB_MIN_CONTENT:
        put(sb, "min-content");
        break;
    case EMBI_TB_MAX_CONTENT:
        put(sb, "max-content");
        break;
    case EMBI_TB_FR:
        put_num(sb, b->v.num);
        put(sb, "fr");
        break;
    default:
        put_val(sb, &b->v);
        break;
    }
}

static void put_track_size(embi_sb *sb, const embi_track_size *t)
{
    if (t->kind == EMBI_TS_MINMAX) {
        put(sb, "minmax(");
        put_breadth(sb, &t->min);
        put(sb, ", ");
        put_breadth(sb, &t->max);
        embi_sb_append_char(sb, ')');
    } else if (t->kind == EMBI_TS_FIT_CONTENT) {
        put(sb, "fit-content(");
        put_breadth(sb, &t->max);
        embi_sb_append_char(sb, ')');
    } else {
        put_breadth(sb, &t->min);
    }
}

static void put_items(embi_sb *sb, const embi_grid_item *items, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (i) {
            embi_sb_append_char(sb, ' ');
        }
        const embi_grid_item *it = &items[i];
        if (it->kind == EMBI_GI_NAMES) {
            embi_sb_append_char(sb, '[');
            for (uint32_t k = 0; k < it->n; k++) {
                if (k) {
                    embi_sb_append_char(sb, ' ');
                }
                embi_sb_append(sb, embi_atom_str(it->names[k]));
            }
            embi_sb_append_char(sb, ']');
        } else if (it->kind == EMBI_GI_TRACK) {
            put_track_size(sb, &it->size);
        } else {
            put(sb, "repeat(");
            if (it->rep == EMBI_GR_AUTO_FILL) {
                put(sb, "auto-fill");
            } else if (it->rep == EMBI_GR_AUTO_FIT) {
                put(sb, "auto-fit");
            } else {
                put_num(sb, it->count);
            }
            put(sb, ", ");
            put_items(sb, it->items, it->n);
            embi_sb_append_char(sb, ')');
        }
    }
}

/* ------------------------------------------------------------------ entry */

void embi_serialize_compound(embi_sb *sb, const embi_value *v)
{
    switch (v->kind) {
    case EMBI_V_RATIO:
        put_num(sb, v->ratio.a);
        if (v->ratio.b != 1.0) {
            put(sb, " / ");
            put_num(sb, v->ratio.b);
        }
        break;
    case EMBI_V_FUNC: {
        embi_str name = embi_atom_str(v->func.name);
        embi_sb_append(sb, name);
        embi_sb_append_char(sb, '(');
        bool attr = embi_str_is(name, "attr");
        bool csize = embi_str_is(name, "calc-size");
        for (uint32_t i = 0; i < v->func.n; i++) {
            const embi_value *arg = &v->func.args[i];
            if (i) {
                put(sb, attr && arg->kind != EMBI_V_RAW ? " " : ", ");
            }
            if (csize && i == 1 && arg->kind == EMBI_V_CALC) {
                /* The calc-sum of calc-size() is written bare (no calc() wrapper). */
                embi_calc_serialize(sb, arg->calc, true);
                continue;
            }
            put_val(sb, arg);
        }
        embi_sb_append_char(sb, ')');
        break;
    }
    case EMBI_V_IMAGE:
        put_image(sb, v->image);
        break;
    case EMBI_V_POSITION:
        put_position(sb, v->pos);
        break;
    case EMBI_V_POS_AXIS:
        put_axis(sb, v->axis, true);
        break;
    case EMBI_V_TRANSFORM:
        for (uint32_t i = 0; i < v->xf.n; i++) {
            if (i) {
                embi_sb_append_char(sb, ' ');
            }
            put(sb, xf_name(v->xf.p[i].fn));
            embi_sb_append_char(sb, '(');
            for (uint32_t k = 0; k < v->xf.p[i].n; k++) {
                if (k) {
                    put(sb, ", ");
                }
                put_val(sb, &v->xf.p[i].args[k]);
            }
            embi_sb_append_char(sb, ')');
        }
        break;
    case EMBI_V_SHADOWS:
        for (uint32_t i = 0; i < v->shadows.n; i++) {
            if (i) {
                put(sb, ", ");
            }
            put_shadow(sb, &v->shadows.p[i], false);
        }
        break;
    case EMBI_V_FILTERS:
        for (uint32_t i = 0; i < v->filters.n; i++) {
            if (i) {
                embi_sb_append_char(sb, ' ');
            }
            put_filter(sb, &v->filters.p[i]);
        }
        break;
    case EMBI_V_SHAPE:
        if (v->shape) {
            put_shape(sb, v->shape);
        }
        if (v->kw) {
            if (v->shape) {
                embi_sb_append_char(sb, ' ');
            }
            put(sb, embi_kw_name(v->kw));
        }
        break;
    case EMBI_V_EASING:
        put_easing(sb, v->easing);
        break;
    case EMBI_V_TRACKS:
        if (v->tracks->subgrid) {
            put(sb, v->tracks->n ? "subgrid " : "subgrid");
        }
        put_items(sb, v->tracks->items, v->tracks->n);
        break;
    case EMBI_V_GRID_LINE: {
        const embi_grid_line *g = v->gline;
        bool sp = false;
        if (g->span) {
            put(sb, "span");
            sp = true;
        }
        if (g->num != 0 && !(g->span && g->num == 1 && g->name != 0)) {
            if (sp) {
                embi_sb_append_char(sb, ' ');
            }
            put_num(sb, g->num);
            sp = true;
        }
        if (g->name) {
            if (sp) {
                embi_sb_append_char(sb, ' ');
            }
            embi_sb_append(sb, embi_atom_str(g->name));
        }
        break;
    }
    default:
        break;
    }
}
