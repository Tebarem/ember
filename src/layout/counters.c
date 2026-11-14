/*
 * CSS counters and quotes (CSS Lists 3 section 4, CSS 2.1 section 12.4, W3a section 1). The layout
 * walks the tree once in document order and keeps the counter instances in scope on a stack.
 *
 * Scope. An element's counter-reset creates an instance that is visible to the element, its
 * following siblings and their descendants. The instances a child creates are dropped when the
 * parent ends, which the box builder does with embi_ctr_mark and embi_ctr_truncate: the mark is
 * taken after the element's own resets, so they survive for its following siblings.
 *
 * Limits. counters(name) lists the values of every instance in scope with that name (the reversed
 * flag is kept, and <ol reversed> counts its items). Counter styles beyond the ones in the
 * predefined set fall back to decimal.
 */
#include "layout_int.h"

#include "css/props.h"

#include <SDL3/SDL_stdinc.h>
#include <stdatomic.h>
#include <string.h>

uint32_t embi_ctr_mark(const embi_ctrs *c)
{
    return c->st.len;
}

void embi_ctr_truncate(embi_ctrs *c, uint32_t mark)
{
    if (mark < c->st.len) {
        c->st.len = mark;
    }
}

/* CSS Lists 3 section 4.4.1 (inheriting counters): the counters an element created are inherited by its
 * following siblings only when the parent's inherited set has no counter of that name. Drops the instances at
 * or above start (the element's own) whose name also has an instance below base (the ancestors' set). The
 * list-item counter and reversed counters (counter-reset-reversed-siblings-003) keep the CSS 2.1 scope (HTML's explicit scope of li: counter-list-item-2), so a nested
 * list's counter stays visible to the items after it; keep is its name. */
void embi_ctr_end_element(embi_ctrs *c, uint32_t start, uint32_t base, embi_atom keep)
{
    uint32_t w = start;
    for (uint32_t i = start; i < c->st.len; i++) {
        bool shadowed = false;
        for (uint32_t j = 0; c->st.data[i].name != keep && c->st.data[i].dir > 0 && j < base && j < start; j++) {
            if (c->st.data[j].name == c->st.data[i].name) {
                shadowed = true;
                break;
            }
        }
        if (!shadowed) {
            c->st.data[w++] = c->st.data[i];
        }
    }
    c->st.len = w;
}

void embi_ctr_free(embi_ctrs *c)
{
    embi_vec_free(&c->st);
    c->quote_depth = 0;
    c->sib_base = 0;
}

static embi_ctr_inst *find_top(const embi_ctrs *c, embi_atom name)
{
    for (uint32_t i = c->st.len; i > 0; i--) {
        embi_ctr_inst *in = &c->st.data[i - 1];
        if (in->name == name) {
            return in;
        }
    }
    return nullptr;
}

/* A written counter amount, saturated like counter values (at 2.1e9) so that no cast or sum overflows. */
static int32_t ctr_num(double d)
{
    return d > 2100000000.0 ? 2100000000 : (d < -2100000000.0 ? -2100000000 : (int32_t)d);
}

static void push(embi_ctrs *c, embi_atom name, int32_t value, int32_t dir)
{
    embi_vec_reserve(&c->st, c->st.len + 1);
    c->st.data[c->st.len++] = (embi_ctr_inst){name, value, dir};
}

int32_t embi_ctr_value(const embi_ctrs *c, embi_atom name)
{
    const embi_ctr_inst *in = find_top(c, name);
    return in != nullptr ? in->value : 0;
}

void embi_ctr_force(embi_ctrs *c, embi_atom name, int32_t value, int32_t dir)
{
    embi_ctr_inst *in = find_top(c, name);
    if (in == nullptr) {
        push(c, name, value, dir);
        return;
    }
    in->value = value;
    in->dir = dir;
}

/* ASCII case-insensitive comparison with a lowercase literal. */
static bool is_lit(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = s.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (ch != lit[i]) {
            return false;
        }
    }
    return true;
}

/* One counter name of a counter-* list: an ident, or reversed(ident). */
static bool pair_name(const embi_value *v, embi_atom *name, bool *reversed)
{
    *reversed = false;
    if (v->kind == EMBI_V_IDENT) {
        *name = v->ident;
        return true;
    }
    if (v->kind == EMBI_V_FUNC && v->func.n >= 1 && v->func.args[0].kind == EMBI_V_IDENT) {
        embi_str fn = embi_atom_str(v->func.name);
        if (is_lit(fn, "reversed")) {
            *name = v->func.args[0].ident;
            *reversed = true;
            return true;
        }
    }
    return false;
}

/* The integer of a counter pair's second item: an <integer>, or a constant calc() of numbers (rounded as CSS Values 4
 * section 10.9.1 says: halves toward positive infinity). Returns false when the item is not a number. */
static bool pair_int(const embi_value *v, int32_t *out)
{
    double x;
    if (v->kind == EMBI_V_INTEGER || v->kind == EMBI_V_NUMBER) {
        x = v->num;
    } else if (v->kind == EMBI_V_CALC && embi_calc_is_constant(v->calc)) {
        embi_calc_env env = {0};
        x = embi_calc_eval(v->calc, &env);
        if (x != x) {
            return false;
        }
        x = __builtin_floor(x + 0.5);
    } else {
        return false;
    }
    *out = x > (double)INT32_MAX ? INT32_MAX : (x < (double)INT32_MIN ? INT32_MIN : (int32_t)x);
    return true;
}

/* Calls fn for each (name, integer) pair of a counter-reset, counter-set or counter-increment
 * value. */
typedef void (*pair_fn)(embi_ctrs *c, embi_atom name, bool reversed, int32_t num, int which);

static void each_pair(embi_ctrs *c, const embi_value *v, pair_fn fn, int which)
{
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return;
    }
    uint32_t n = 0;
    const embi_value *items = embi_value_items(v, &n);
    bool wrapped = v->kind == EMBI_V_LIST && n > 0 && items[0].kind == EMBI_V_LIST;
    if (!wrapped) {
        /* One pair, not wrapped in a list of pairs. */
        n = 1;
        items = v;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *pair = wrapped ? &items[i] : items;
        uint32_t pn = 0;
        const embi_value *pv = nullptr;
        if (pair->kind == EMBI_V_LIST) {
            pv = embi_value_items(pair, &pn);
        } else {
            pv = pair;
            pn = 1;
        }
        if (pn == 0) {
            continue;
        }
        embi_atom name = 0;
        bool reversed = false;
        if (!pair_name(&pv[0], &name, &reversed)) {
            continue;
        }
        int32_t num = 0;
        if (pn >= 2 && pair_int(&pv[1], &num)) {
            num = ctr_num((double)num); /* W4g4b: counters saturate at +-2.1e9 like the WPT overflow test expects */
        }
        fn(c, name, reversed, num, which == 0 && pn >= 2 && pv[1].kind == EMBI_V_INTEGER ? 1 : which);
    }
}

bool embi_ctr_style_op(const embi_style *s, embi_prop_id prop, embi_atom name, int32_t *num, bool *has_num)
{
    const embi_value *v = embi_style_value(s, prop);
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return false;
    }
    uint32_t n = 0;
    const embi_value *items = embi_value_items(v, &n);
    bool wrapped = v->kind == EMBI_V_LIST && n > 0 && items[0].kind == EMBI_V_LIST;
    if (!wrapped) {
        n = 1;
        items = v;
    }
    bool found = false;
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *pair = wrapped ? &items[i] : items;
        uint32_t pn = 0;
        const embi_value *pv = pair;
        pn = 1;
        if (pair->kind == EMBI_V_LIST) {
            pv = embi_value_items(pair, &pn);
        }
        embi_atom nm = 0;
        bool rev = false;
        if (pn == 0 || !pair_name(&pv[0], &nm, &rev) || nm != name) {
            continue;
        }
        found = true;
        *has_num = pn >= 2 && pv[1].kind == EMBI_V_INTEGER;
        *num = *has_num ? ctr_num(pv[1].num) : 0;
    }
    return found;
}

/* A counter-reset. An instance of the same name created by a preceding sibling (at or above
 * sib_base) is replaced, because the new one is the one in scope for the following siblings
 * (CSS Lists 3 section 4.4). Otherwise a new instance is pushed. */
static void do_reset(embi_ctrs *c, embi_atom name, bool reversed, int32_t num, int which)
{
    /* which is 1 when the reset gave a value; a reversed counter without one counts down to 1 over its scope. */
    if (reversed && which != 1 && c->lctx != nullptr) {
        num = embi_ctr_reversed_initial(c, name);
    }
    for (uint32_t i = c->st.len; i > c->sib_base; i--) {
        embi_ctr_inst *in = &c->st.data[i - 1];
        if (in->name == name) {
            in->value = num;
            in->dir = reversed ? -1 : 1;
            return;
        }
    }
    push(c, name, num, reversed ? -1 : 1);
}

static void do_set(embi_ctrs *c, embi_atom name, bool reversed, int32_t num, int which)
{
    (void)which;
    (void)reversed;
    embi_ctr_inst *in = find_top(c, name);
    if (in != nullptr) {
        in->value = num;
    } else {
        push(c, name, num, 1);
    }
}

static bool has_explicit_increment(const embi_value *v, embi_atom list_item)
{
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return false;
    }
    uint32_t n = 0;
    const embi_value *items = embi_value_items(v, &n);
    bool wrapped = v->kind == EMBI_V_LIST && n > 0 && items[0].kind == EMBI_V_LIST;
    if (!wrapped) {
        items = v;
        n = 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *pair = wrapped ? &items[i] : items;
        uint32_t pn = 0;
        const embi_value *pv = pair->kind == EMBI_V_LIST ? embi_value_items(pair, &pn) : pair;
        if (pair->kind != EMBI_V_LIST) {
            pn = 1;
        }
        embi_atom name = 0;
        bool reversed = false;
        if (pn > 0 && pair_name(&pv[0], &name, &reversed) && name == list_item) {
            return true;
        }
    }
    return false;
}

/* Applies one increment (num is the amount) to the counter, creating it when none is in scope. */
static void do_inc(embi_ctrs *c, embi_atom name, int32_t num, bool implicit)
{
    embi_ctr_inst *in = find_top(c, name);
    if (in == nullptr) {
        push(c, name, 0, 1);
        in = &c->st.data[c->st.len - 1];
    }
    /* Written amounts apply as they are; only the implicit list-item increment follows the direction. */
    int64_t nv = (int64_t)in->value + (int64_t)num * (implicit ? in->dir : 1);
    /* A counter saturates at 2.1e9 (CSS Lists 3 allows a limit; WPT counter-reset-increment-overflow-underflow.html). */
    in->value = nv > 2100000000LL ? 2100000000 : (nv < -2100000000LL ? -2100000000 : (int32_t)nv);
}

void embi_ctr_apply(embi_ctrs *c, const embi_style *s, bool implicit_list, bool has_value,
                    int32_t value)
{
    if (s == nullptr) {
        return;
    }
    const embi_value *reset = embi_style_value(s, EMBI_PROP_counter_reset);
    const embi_value *set = embi_style_value(s, EMBI_PROP_counter_set);
    const embi_value *inc = embi_style_value(s, EMBI_PROP_counter_increment);
    /* The common element touches no counter: leave before the atom table (a lock and a hash) is asked. */
    if (!implicit_list && !has_value && (reset == nullptr || reset->kind == EMBI_V_KEYWORD) &&
        (set == nullptr || set->kind == EMBI_V_KEYWORD) && (inc == nullptr || inc->kind == EMBI_V_KEYWORD)) {
        return;
    }
    static _Atomic embi_atom li_atom;
    embi_atom li = atomic_load_explicit(&li_atom, memory_order_relaxed);
    if (li == 0) {
        li = embi_atom_intern("list-item", 9);
        atomic_store_explicit(&li_atom, li, memory_order_relaxed);
    }

    each_pair(c, reset, do_reset, 0);

    /* The increments of the element: explicit ones, then the implicit list-item one. */
    if (inc != nullptr && inc->kind != EMBI_V_KEYWORD) {
        uint32_t n = 0;
        const embi_value *items = embi_value_items(inc, &n);
        bool wrapped = inc->kind == EMBI_V_LIST && n > 0 && items[0].kind == EMBI_V_LIST;
        if (!wrapped) {
            items = inc;
            n = 1;
        }
        for (uint32_t i = 0; i < n; i++) {
            const embi_value *pair = wrapped ? &items[i] : items;
            uint32_t pn = 0;
            const embi_value *pv = pair->kind == EMBI_V_LIST ? embi_value_items(pair, &pn) : pair;
            if (pair->kind != EMBI_V_LIST) {
                pn = 1;
            }
            embi_atom name = 0;
            bool reversed = false;
            if (pn == 0 || !pair_name(&pv[0], &name, &reversed)) {
                continue;
            }
            int32_t num = 1;
            if (pn >= 2 && pair_int(&pv[1], &num)) {
                num = ctr_num((double)num); /* W4g4b: counters saturate at +-2.1e9 like the WPT overflow test expects */
            }
            do_inc(c, name, num, false);
        }
    }
    if (implicit_list && !has_explicit_increment(inc, li)) {
        do_inc(c, li, 1, true);
    }
    /* The value attribute of a list item is a counter-set of list-item: it applies after the increments, whatever they were. */
    if (has_value) {
        const embi_ctr_inst *cur = find_top(c, li);
        embi_ctr_force(c, li, ctr_num(value), cur != nullptr ? cur->dir : 1);
    }
    /* counter-set applies after the increments (CSS Lists 3 section 4.5). */
    each_pair(c, set, do_set, 2);
}

/* ------------------------------------------------------------------ formatting */

static void append_bytes(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static void format_roman(embi_sb *sb, int32_t v, bool upper)
{
    static const struct {
        int32_t v;
        const char *s;
    } tab[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
               {50, "l"},   {40, "xl"},  {10, "x"}, {9, "ix"},   {5, "v"},  {4, "iv"}, {1, "i"}};
    for (uint32_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        while (v >= tab[i].v) {
            v -= tab[i].v;
            for (const char *p = tab[i].s; *p; p++) {
                embi_sb_append_char(sb, upper ? (char)(*p - 'a' + 'A') : *p);
            }
        }
    }
}

static void format_alpha(embi_sb *sb, int32_t v, bool upper)
{
    char buf[16];
    uint32_t n = 0;
    uint32_t x = (uint32_t)v;
    while (x > 0) {
        x--;
        buf[n++] = (char)((upper ? 'A' : 'a') + (x % 26));
        x /= 26;
    }
    while (n > 0) {
        embi_sb_append_char(sb, buf[--n]);
    }
}

/* An additive counter style (CSS Counter Styles 3 section 3.1.3): the weights from large to small with their code points. */
typedef struct addw {
    int32_t w;
    uint32_t cp;
} addw;

static void format_additive(embi_sb *sb, int32_t v, const addw *tab, uint32_t n)
{
    for (uint32_t i = 0; i < n && v > 0; i++) {
        while (v >= tab[i].w) {
            v -= tab[i].w;
            embi_sb_append_cp(sb, tab[i].cp);
        }
    }
}

/* georgian: 1 to 19999. armenian: 1 to 9999 (upper-armenian). */
static const addw k_georgian[] = {
    {10000, 0x10F5}, {9000, 0x10F0}, {8000, 0x10EF}, {7000, 0x10F4}, {6000, 0x10EE}, {5000, 0x10ED}, {4000, 0x10EC},
    {3000, 0x10EB},  {2000, 0x10EA}, {1000, 0x10E9}, {900, 0x10E8},  {800, 0x10E7},  {700, 0x10E6},  {600, 0x10E5},
    {500, 0x10E4},   {400, 0x10F3},  {300, 0x10E2},  {200, 0x10E1},  {100, 0x10E0},  {90, 0x10DF},   {80, 0x10DE},
    {70, 0x10DD},    {60, 0x10F2},   {50, 0x10DC},   {40, 0x10DB},   {30, 0x10DA},   {20, 0x10D9},   {10, 0x10D8},
    {9, 0x10D7},     {8, 0x10F1},    {7, 0x10D6},    {6, 0x10D5},    {5, 0x10D4},    {4, 0x10D3},    {3, 0x10D2},
    {2, 0x10D1},     {1, 0x10D0},
};
static const addw k_armenian[] = {
    {9000, 0x554}, {8000, 0x553}, {7000, 0x552}, {6000, 0x551}, {5000, 0x550}, {4000, 0x54F}, {3000, 0x54E},
    {2000, 0x54D}, {1000, 0x54C}, {900, 0x54B},  {800, 0x54A},  {700, 0x549},  {600, 0x548},  {500, 0x547},
    {400, 0x546},  {300, 0x545},  {200, 0x544},  {100, 0x543},  {90, 0x542},   {80, 0x541},   {70, 0x540},
    {60, 0x53F},   {50, 0x53E},   {40, 0x53D},   {30, 0x53C},   {20, 0x53B},   {10, 0x53A},   {9, 0x539},
    {8, 0x538},    {7, 0x537},    {6, 0x536},    {5, 0x535},    {4, 0x534},    {3, 0x533},    {2, 0x532},
    {1, 0x531},
};

uint16_t embi_ctr_style_name(embi_str name)
{
    static const char *const open = "disclosure-open";
    static const char *const closed = "disclosure-closed";
    if (name.len == strlen(open) && is_lit(name, open)) {
        return EMBI_CTR_DISCLOSURE_OPEN;
    }
    if (name.len == strlen(closed) && is_lit(name, closed)) {
        return EMBI_CTR_DISCLOSURE_CLOSED;
    }
    return embi_kw_lookup(name);
}

void embi_ctr_format(embi_sb *sb, int32_t v, uint16_t style_kw)
{
    char num[16];
    switch (style_kw) {
    case EMBI_KW_none:
        return;
    case EMBI_CTR_DISCLOSURE_OPEN:
        embi_sb_append(sb, (embi_str){"\xE2\x96\xBE", 3}); /* U+25BE BLACK DOWN-POINTING SMALL TRIANGLE */
        return;
    case EMBI_CTR_DISCLOSURE_CLOSED:
        embi_sb_append(sb, (embi_str){"\xE2\x96\xB8", 3}); /* U+25B8 BLACK RIGHT-POINTING SMALL TRIANGLE */
        return;
    case EMBI_CTR_DISCLOSURE_CLOSED_RTL:
        embi_sb_append(sb, (embi_str){"\xE2\x97\x82", 3}); /* U+25C2 BLACK LEFT-POINTING SMALL TRIANGLE */
        return;
    case EMBI_KW_disc:
        embi_sb_append(sb, (embi_str){"\xE2\x80\xA2", 3});
        return;
    case EMBI_KW_circle:
        embi_sb_append(sb, (embi_str){"\xE2\x97\xA6", 3});
        return;
    case EMBI_KW_square:
        embi_sb_append(sb, (embi_str){"\xE2\x96\xAA", 3});
        return;
    case EMBI_KW_lower_roman:
    case EMBI_KW_upper_roman:
        if (v >= 1 && v <= 3999) {
            format_roman(sb, v, style_kw == EMBI_KW_upper_roman);
            return;
        }
        break;
    case EMBI_KW_georgian:
        if (v >= 1 && v <= 19999) {
            format_additive(sb, v, k_georgian, (uint32_t)SDL_arraysize(k_georgian));
            return;
        }
        break;
    case EMBI_KW_armenian:
        if (v >= 1 && v <= 9999) {
            format_additive(sb, v, k_armenian, (uint32_t)SDL_arraysize(k_armenian));
            return;
        }
        break;
    case EMBI_KW_lower_alpha:
    case EMBI_KW_lower_latin:
    case EMBI_KW_upper_alpha:
    case EMBI_KW_upper_latin:
        if (v >= 1) {
            format_alpha(sb, v, style_kw == EMBI_KW_upper_alpha || style_kw == EMBI_KW_upper_latin);
            return;
        }
        break;
    case EMBI_KW_lower_greek:
        if (v >= 1 && v <= 24) {
            /* alpha..rho (U+03B1..U+03C1), then sigma..omega (U+03C3..U+03C9). */
            uint32_t cp = v <= 17 ? 0x03B0u + (uint32_t)v : 0x03C3u + (uint32_t)(v - 18);
            embi_sb_append_cp(sb, cp);
            return;
        }
        break;
    default:
        break;
    }
    if (style_kw == EMBI_KW_decimal_leading_zero && v > -10 && v < 10) {
        SDL_snprintf(num, sizeof num, "%s0%d", v < 0 ? "-" : "", v < 0 ? -v : v);
    } else {
        SDL_snprintf(num, sizeof num, "%d", v);
    }
    append_bytes(sb, num);
}

/* The quotation marks of `quotes: auto` for a language (CSS Content 3 section 3.2; the pairs follow CLDR, as the HTML
 * rendering section lists them): the outer and inner pair. Deeper levels use the guillemets and then the single ones.
 * The lang is a BCP 47 tag; unknown languages get the English marks. */
typedef struct quote_lang {
    const char *tag; /* primary language, or language-region / language-script for the special cases (lowercase) */
    const char *q[4];
} quote_lang;

#define Q_LDQ "\xE2\x80\x9C"
#define Q_RDQ "\xE2\x80\x9D"
#define Q_LSQ "\xE2\x80\x98"
#define Q_RSQ "\xE2\x80\x99"
#define Q_LDQ_LOW "\xE2\x80\x9E"
#define Q_LSQ_LOW "\xE2\x80\x9A"
#define Q_LG "\xC2\xAB"
#define Q_RG "\xC2\xBB"
#define Q_LSG "\xE2\x80\xB9"
#define Q_RSG "\xE2\x80\xBA"
#define Q_LCB "\xE3\x80\x8C"
#define Q_RCB "\xE3\x80\x8D"
#define Q_LWCB "\xE3\x80\x8E"
#define Q_RWCB "\xE3\x80\x8F"

static const quote_lang k_quote_langs[] = {
    {"fr-ch", {Q_LG, Q_RG, Q_LSG, Q_RSG}},
    {"pt-pt", {Q_LG, Q_RG, Q_LDQ, Q_RDQ}},
    {"zh-hant", {Q_LCB, Q_RCB, Q_LWCB, Q_RWCB}},
    {"zh-tw", {Q_LCB, Q_RCB, Q_LWCB, Q_RWCB}},
    {"zh-hk", {Q_LCB, Q_RCB, Q_LWCB, Q_RWCB}},
    {"af", {Q_LDQ, Q_RDQ, Q_LSQ, Q_RSQ}},
    {"am", {Q_LG, Q_RG, Q_LSG, Q_RSG}},
    {"ar", {Q_RDQ, Q_LDQ, Q_RSQ, Q_LSQ}},
    {"be", {Q_LG, Q_RG, Q_LDQ_LOW, Q_LDQ}},
    {"bg", {Q_LDQ_LOW, Q_LDQ, Q_LSQ_LOW, Q_LSQ}},
    {"ca", {Q_LG, Q_RG, Q_LDQ, Q_RDQ}},
    {"cs", {Q_LDQ_LOW, Q_LDQ, Q_LSQ_LOW, Q_LSQ}},
    {"da", {Q_RG, Q_LG, Q_RSG, Q_LSG}},
    {"de", {Q_LDQ_LOW, Q_LDQ, Q_LSQ_LOW, Q_LSQ}},
    {"el", {Q_LG, Q_RG, Q_LDQ, Q_RDQ}},
    {"es", {Q_LG, Q_RG, Q_LDQ, Q_RDQ}},
    {"et", {Q_LDQ_LOW, Q_LDQ, Q_LSQ_LOW, Q_LSQ}},
    {"fa", {Q_LG, Q_RG, Q_LSG, Q_RSG}},
    {"fi", {Q_RDQ, Q_RDQ, Q_RSQ, Q_RSQ}},
    {"fr", {Q_LG, Q_RG, Q_LG, Q_RG}},
    {"he", {Q_RDQ, Q_RDQ, Q_RSQ, Q_RSQ}},
    {"hu", {Q_LDQ_LOW, Q_RDQ, Q_RG, Q_LG}},
    {"it", {Q_LG, Q_RG, Q_LDQ, Q_RDQ}},
    {"ja", {Q_LCB, Q_RCB, Q_LWCB, Q_RWCB}},
    {"lt", {Q_LDQ_LOW, Q_LDQ, Q_LDQ_LOW, Q_LDQ}},
    {"lv", {Q_LG, Q_RG, Q_LDQ_LOW, Q_LDQ}},
    {"nb", {Q_LG, Q_RG, Q_LSQ, Q_RSQ}},
    {"nn", {Q_LG, Q_RG, Q_LSQ, Q_RSQ}},
    {"no", {Q_LG, Q_RG, Q_LSQ, Q_RSQ}},
    {"nl", {Q_LSQ, Q_RSQ, Q_LSQ, Q_RSQ}},
    {"pl", {Q_LDQ_LOW, Q_RDQ, Q_LG, Q_RG}},
    {"ro", {Q_LDQ_LOW, Q_RDQ, Q_LG, Q_RG}},
    {"ru", {Q_LG, Q_RG, Q_LDQ_LOW, Q_LDQ}},
    {"sk", {Q_LDQ_LOW, Q_LDQ, Q_LSQ_LOW, Q_LSQ}},
    {"sl", {Q_LDQ_LOW, Q_LDQ, Q_LSQ_LOW, Q_LSQ}},
    {"sv", {Q_RDQ, Q_RDQ, Q_RSQ, Q_RSQ}},
    {"uk", {Q_LG, Q_RG, Q_LDQ_LOW, Q_LDQ}},
};

/* True when the lowercase tag of the table equals the language tag lang or its prefix up to a subtag boundary. */
static bool quote_tag_matches(const char *tag, embi_str lang)
{
    size_t tn = strlen(tag);
    if (lang.len < tn) {
        return false;
    }
    for (size_t i = 0; i < tn; i++) {
        char ch = lang.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch + 32);
        }
        if (ch != tag[i]) {
            return false;
        }
    }
    return lang.len == tn || lang.ptr[tn] == '-' || lang.ptr[tn] == '_';
}

#define QUOTE_PAIRS_MAX 8u /* the pairs of an explicit quotes list that are kept (deeper levels use the last) */

static void quote_auto(embi_str lang, uint32_t depth, bool open, embi_sb *sb)
{
    const char *const *q = nullptr;
    for (size_t i = 0; i < SDL_arraysize(k_quote_langs) && q == nullptr; i++) {
        if (quote_tag_matches(k_quote_langs[i].tag, lang)) {
            q = k_quote_langs[i].q;
        }
    }
    static const char *const en[4] = {Q_LDQ, Q_RDQ, Q_LSQ, Q_RSQ};
    if (q == nullptr) {
        q = en;
    }
    /* Levels past the language's two pairs repeat the last one (CSS Content 3 section 3.1: "the last pair is used";
     * quotes-030: the third level of Japanese is the second pair again). */
    uint32_t lv = depth < 2 ? depth : 1;
    const char *s = q[2 * lv + (open ? 0 : 1)];
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

void embi_ctr_quote(embi_ctrs *c, const embi_value *quotes, bool open, embi_str lang, embi_sb *sb)
{
    bool none = false;
    uint32_t npairs = 2;
    embi_str pstr[2 * QUOTE_PAIRS_MAX];
    memset(pstr, 0, sizeof pstr);
    bool use_def = quotes == nullptr || (quotes->kind == EMBI_V_KEYWORD && quotes->kw == EMBI_KW_auto);
    if (quotes != nullptr && quotes->kind == EMBI_V_KEYWORD && quotes->kw == EMBI_KW_none) {
        none = true;
    }
    if (use_def) {
        if (open) {
            quote_auto(lang, c->quote_depth, true, sb);
            c->quote_depth++;
        } else if (c->quote_depth > 0) {
            c->quote_depth--;
            quote_auto(lang, c->quote_depth, false, sb);
        }
        return;
    }
    if (!none) {
        uint32_t n = 0;
        const embi_value *items = embi_value_items(quotes, &n);
        npairs = n / 2;
        if (npairs == 0) {
            npairs = 1;
        }
        if (npairs > QUOTE_PAIRS_MAX) {
            npairs = QUOTE_PAIRS_MAX;
        }
        /* Entries past the list are empty strings (a one-pair list still gets its close quote). */
        for (uint32_t i = 0; i < 2 * npairs && i < n; i++) {
            pstr[i] = items[i].str;
        }
    }
    if (none) {
        if (open) {
            c->quote_depth++;
        } else if (c->quote_depth > 0) {
            c->quote_depth--;
        }
        return;
    }
    if (open) {
        uint32_t lvl = c->quote_depth < npairs ? c->quote_depth : npairs - 1;
        embi_sb_append(sb, pstr[2 * lvl]);
        c->quote_depth++;
        return;
    }
    if (c->quote_depth == 0) {
        return;
    }
    c->quote_depth--;
    uint32_t lvl = c->quote_depth < npairs ? c->quote_depth : npairs - 1;
    embi_sb_append(sb, pstr[2 * lvl + 1]);
}
