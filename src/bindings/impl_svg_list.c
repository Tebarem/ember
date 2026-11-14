/*
 * The lists and the animated wrappers of the SVG DOM (R4d).
 *
 * A list (SVGLengthList, SVGNumberList, SVGPointList, SVGTransformList) is a view of its element (aux = the attribute,
 * owned = the sv of the list). It keeps its items as objects with identity: ref is an array of item wrappers and
 * u.list.n their number. embi_svg_list_sync parses the attribute again when its text changed and updates the held items
 * in place (extra items are dropped from the list and detached, new ones are made); embi_svg_list_commit serializes the
 * items into the attribute. An item that is already in a list is copied on insertion. The animVal of a list is the
 * base list (no SMIL). SVGStringList has no item objects: every operation parses and writes the attribute.
 */
#include "bindings/svg_int.h"
#include "num/num.h"

#include <stdint.h>

static embi_str cstr(const char *s)
{
    return (embi_str){s, (uint32_t)SDL_strlen(s)};
}

static bool svg_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* ------------------------------------------------------------------ list views */

static void list_release(void *owned)
{
    sv *p = owned;
    if (p->u.list.last != nullptr) {
        embi_free(p->u.list.last, p->u.list.last_len);
        p->u.list.last = nullptr;
    }
}

static uint8_t ikind_of_iface(uint16_t iface)
{
    switch (iface) {
    case EMBI_IF_SVGLengthList:
        return SV_LEN;
    case EMBI_IF_SVGNumberList:
        return SV_NUM;
    case EMBI_IF_SVGPointList:
        return SV_PT;
    case EMBI_IF_SVGTransformList:
        return SV_TF;
    default:
        return SV_STR;
    }
}

embi_js_value embi_svg_list_view(embi_bind_ctx *c, embi_node *self, uint16_t iface, uint32_t idx, bool ro)
{
    embi_bind_obj *owner = embi_impl_node_rec(c, self);
    sv *p = embi_alloc(sizeof *p);
    SDL_memset(p, 0, sizeof *p);
    p->kind = SV_LIST;
    p->ikind = ikind_of_iface(iface);
    p->attr = (uint16_t)idx;
    p->ro = ro;
    embi_js_value v = embi_bind_view_new(c, iface, owner, idx, p, sizeof *p);
    p->self = embi_bind_obj_of(v);
    p->self->release = list_release;
    return v;
}

static sv *plist(embi_bind_obj *L)
{
    return L->owned;
}

static sv *item_at(embi_bind_ctx *c, embi_bind_obj *L, uint32_t i)
{
    embi_js_value v = embi_bind_array_get(c, L->ref, i);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    return bo ? bo->ptr : nullptr;
}

static void set_last(sv *pl, embi_str t)
{
    if (pl->u.list.last != nullptr) {
        embi_free(pl->u.list.last, pl->u.list.last_len);
        pl->u.list.last = nullptr;
        pl->u.list.last_len = 0;
    }
    if (t.len > 0) {
        pl->u.list.last = embi_alloc(t.len);
        SDL_memcpy(pl->u.list.last, t.ptr, t.len);
        pl->u.list.last_len = t.len;
    }
    pl->u.list.have = true;
}

static void ensure_array(embi_bind_ctx *c, embi_bind_obj *L)
{
    if (embi_js_is_empty(L->ref)) {
        embi_bind_set_ref(L, embi_bind_array_new(c, 0));
    }
}

/* A new item attached to the list, holding the data of src. */
static embi_js_value item_new(embi_bind_ctx *c, embi_bind_obj *L, const sv *src)
{
    sv *pl = plist(L);
    embi_js_value v;
    sv *it = embi_sv_new(c, embi_sv_iface_of(pl->ikind), pl->ikind, &v);
    embi_bind_pin(c, v);
    embi_sv_copy_data(it, src);
    it->bound = SB_ITEM;
    it->attr = pl->attr;
    it->ro = pl->ro;
    embi_bind_set_ref(it->self, embi_bind_obj_value(L));
    return v;
}

static void item_detach(sv *it)
{
    if (it != nullptr) {
        it->bound = SB_FREE;
        it->ro = 0;
        embi_bind_set_ref(it->self, EMBI_JS_EMPTY);
    }
}

/* True when no DOM mutation happened since the list was last in step with its attribute: reading an item, the length or a
 * member of an item is then O(1) instead of converting and comparing the whole attribute text (iterating n items costs
 * O(n), not O(n * len)). The epoch counts the mutations of the documents the context observes; any other document is
 * compared by text every time. */
static bool list_current(embi_bind_ctx *c, const sv *pl, const embi_node *n)
{
    return pl->u.list.have && n != nullptr && n->doc == c->doc && pl->u.list.epoch == embi_bind_mut_epoch(c);
}

void embi_svg_list_sync(embi_bind_ctx *c, embi_bind_obj *L)
{
    sv *pl = plist(L);
    if (pl->ikind == SV_STR) {
        return;
    }
    embi_node *n = embi_bind_owner_node(L);
    if (n == nullptr && pl->u.list.have) {
        return;
    }
    if (list_current(c, pl, n)) {
        return;
    }
    embi_str t = {"", 0};
    if (n != nullptr) {
        (void)embi_sv_attr_text(c, n, pl->attr, &t);
    }
    if (pl->u.list.have && t.len == pl->u.list.last_len &&
        (t.len == 0 || SDL_memcmp(t.ptr, pl->u.list.last, t.len) == 0)) {
        pl->u.list.epoch = embi_bind_mut_epoch(c);
        return;
    }
    sv *items = nullptr;
    uint32_t cnt = 0, cap = 0;
    if (!embi_sv_parse_list(t, pl->ikind, pl->attr, &items, &cnt, &cap)) {
        cnt = 0;
    }
    ensure_array(c, L);
    uint32_t old = pl->u.list.n;
    for (uint32_t i = 0; i < cnt; i++) {
        if (i < old) {
            sv *it = item_at(c, L, i);
            if (it != nullptr) {
                embi_sv_copy_data(it, &items[i]);
            }
        } else {
            embi_bind_array_set(c, L->ref, i, item_new(c, L, &items[i]));
        }
    }
    for (uint32_t i = cnt; i < old; i++) {
        item_detach(item_at(c, L, i));
        embi_bind_array_set(c, L->ref, i, EMBI_JS_UNDEFINED);
    }
    pl->u.list.n = cnt;
    set_last(pl, t);
    pl->u.list.epoch = embi_bind_mut_epoch(c);
    embi_sv_free_items(items, cap);
}

void embi_svg_list_commit(embi_bind_ctx *c, embi_bind_obj *L)
{
    sv *pl = plist(L);
    embi_sb sb = {0};
    for (uint32_t i = 0; i < pl->u.list.n; i++) {
        sv *it = item_at(c, L, i);
        if (it == nullptr) {
            continue;
        }
        if (i > 0) {
            embi_sb_append_char(&sb, ' ');
        }
        embi_sv_serialize_item(&sb, it);
    }
    embi_str t = {sb.data ? sb.data : "", sb.len};
    embi_node *n = embi_bind_owner_node(L);
    if (n != nullptr) {
        embi_sv_set_attr(c, n, pl->attr, t);
    }
    set_last(pl, t);
    pl->u.list.epoch = embi_bind_mut_epoch(c); /* after the attribute write: the list is in step with it */
    embi_sb_free(&sb);
}

/* ------------------------------------------------------------------ list operations (item lists) */

static uint32_t lst_len(embi_bind_ctx *c, embi_bind_obj *L)
{
    embi_svg_list_sync(c, L);
    return plist(L)->u.list.n;
}

static embi_dom_err index_error(embi_bind_ctx *c)
{
    return embi_impl_throw(c, "IndexSizeError", "The index is out of range.");
}

/* animVal lists are read only. */
static embi_dom_err ro_list(embi_bind_ctx *c, embi_bind_obj *L)
{
    if (plist(L)->ro) {
        return embi_impl_throw(c, "NoModificationAllowedError", "The list is read-only.");
    }
    return EMBI_DOM_OK;
}

/* The wrapper to put in the list for newItem: the item itself when it is free, a copy when it is in a list already. */
static embi_js_value lst_take(embi_bind_ctx *c, embi_bind_obj *L, sv *src)
{
    if (src->bound == SB_FREE) {
        src->bound = SB_ITEM;
        src->attr = plist(L)->attr;
        embi_bind_set_ref(src->self, embi_bind_obj_value(L));
        return embi_bind_obj_value(src->self);
    }
    embi_sv_load(c, src);
    return item_new(c, L, src);
}

static void lst_clear_items(embi_bind_ctx *c, embi_bind_obj *L)
{
    sv *pl = plist(L);
    for (uint32_t i = 0; i < pl->u.list.n; i++) {
        item_detach(item_at(c, L, i));
        embi_bind_array_set(c, L->ref, i, EMBI_JS_UNDEFINED);
    }
    pl->u.list.n = 0;
}

static void lst_insert(embi_bind_ctx *c, embi_bind_obj *L, uint32_t at, embi_js_value v)
{
    sv *pl = plist(L);
    ensure_array(c, L);
    for (uint32_t j = pl->u.list.n; j > at; j--) {
        embi_bind_array_set(c, L->ref, j, embi_bind_array_get(c, L->ref, j - 1));
    }
    embi_bind_array_set(c, L->ref, at, v);
    pl->u.list.n++;
}

static embi_dom_err lst_clear(embi_bind_ctx *c, embi_bind_obj *L)
{
    embi_dom_err ro_e = ro_list(c, L);
    if (ro_e) {
        return ro_e;
    }
    embi_svg_list_sync(c, L);
    lst_clear_items(c, L);
    embi_svg_list_commit(c, L);
    return EMBI_DOM_OK;
}

static embi_dom_err lst_initialize(embi_bind_ctx *c, embi_bind_obj *L, void *a0, embi_js_value *out)
{
    embi_dom_err ro_e = ro_list(c, L);
    if (ro_e) {
        return ro_e;
    }
    embi_svg_list_sync(c, L);
    embi_js_value v = embi_bind_pin(c, lst_take(c, L, a0));
    lst_clear_items(c, L);
    lst_insert(c, L, 0, v);
    embi_svg_list_commit(c, L);
    *out = v;
    return EMBI_DOM_OK;
}

static embi_dom_err lst_get_item(embi_bind_ctx *c, embi_bind_obj *L, uint32_t i, embi_js_value *out)
{
    if (i >= lst_len(c, L)) {
        return index_error(c);
    }
    *out = embi_bind_array_get(c, L->ref, i);
    return EMBI_DOM_OK;
}

static embi_js_value lst_indexed(embi_bind_ctx *c, embi_bind_obj *L, uint32_t i)
{
    if (i >= lst_len(c, L)) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_array_get(c, L->ref, i);
}

static embi_dom_err lst_insert_before(embi_bind_ctx *c, embi_bind_obj *L, void *a0, uint32_t i, embi_js_value *out)
{
    embi_dom_err ro_e = ro_list(c, L);
    if (ro_e) {
        return ro_e;
    }
    uint32_t n = lst_len(c, L);
    embi_js_value v = embi_bind_pin(c, lst_take(c, L, a0));
    lst_insert(c, L, i > n ? n : i, v);
    embi_svg_list_commit(c, L);
    *out = v;
    return EMBI_DOM_OK;
}

static embi_dom_err lst_replace(embi_bind_ctx *c, embi_bind_obj *L, void *a0, uint32_t i, embi_js_value *out)
{
    embi_dom_err ro_e = ro_list(c, L);
    if (ro_e) {
        return ro_e;
    }
    if (i >= lst_len(c, L)) {
        return index_error(c);
    }
    embi_js_value v = embi_bind_pin(c, lst_take(c, L, a0));
    item_detach(item_at(c, L, i));
    embi_bind_array_set(c, L->ref, i, v);
    embi_svg_list_commit(c, L);
    *out = v;
    return EMBI_DOM_OK;
}

static embi_dom_err lst_remove(embi_bind_ctx *c, embi_bind_obj *L, uint32_t i, embi_js_value *out)
{
    embi_dom_err ro_e = ro_list(c, L);
    if (ro_e) {
        return ro_e;
    }
    sv *pl = plist(L);
    if (i >= lst_len(c, L)) {
        return index_error(c);
    }
    embi_js_value v = embi_bind_pin(c, embi_bind_array_get(c, L->ref, i));
    item_detach(item_at(c, L, i));
    for (uint32_t j = i; j + 1 < pl->u.list.n; j++) {
        embi_bind_array_set(c, L->ref, j, embi_bind_array_get(c, L->ref, j + 1));
    }
    embi_bind_array_set(c, L->ref, pl->u.list.n - 1, EMBI_JS_UNDEFINED);
    pl->u.list.n--;
    embi_svg_list_commit(c, L);
    *out = v;
    return EMBI_DOM_OK;
}

static embi_dom_err lst_append(embi_bind_ctx *c, embi_bind_obj *L, void *a0, embi_js_value *out)
{
    embi_dom_err ro_e = ro_list(c, L);
    if (ro_e) {
        return ro_e;
    }
    uint32_t n = lst_len(c, L);
    embi_js_value v = embi_bind_pin(c, lst_take(c, L, a0));
    lst_insert(c, L, n, v);
    embi_svg_list_commit(c, L);
    *out = v;
    return EMBI_DOM_OK;
}

#define LIST_IFACE(NAME)                                                                                          \
    uint32_t embi_impl_##NAME##_get_length(embi_bind_ctx *c, embi_bind_obj *self)                                  \
    {                                                                                                             \
        return lst_len(c, self);                                                                                  \
    }                                                                                                             \
    uint32_t embi_impl_##NAME##_get_numberOfItems(embi_bind_ctx *c, embi_bind_obj *self)                           \
    {                                                                                                             \
        return lst_len(c, self);                                                                                  \
    }                                                                                                             \
    embi_js_value embi_impl_##NAME##_indexed(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)                   \
    {                                                                                                             \
        return lst_indexed(c, self, a0);                                                                          \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_clear(embi_bind_ctx *c, embi_bind_obj *self)                                   \
    {                                                                                                             \
        return lst_clear(c, self);                                                                                \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_initialize(embi_bind_ctx *c, embi_bind_obj *self, void *a0, embi_js_value *out) \
    {                                                                                                             \
        return lst_initialize(c, self, a0, out);                                                                  \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_getItem(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0, embi_js_value *out) \
    {                                                                                                             \
        return lst_get_item(c, self, a0, out);                                                                    \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_insertItemBefore(embi_bind_ctx *c, embi_bind_obj *self, void *a0, uint32_t a1,  \
                                                     embi_js_value *out)                                          \
    {                                                                                                             \
        return lst_insert_before(c, self, a0, a1, out);                                                           \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_replaceItem(embi_bind_ctx *c, embi_bind_obj *self, void *a0, uint32_t a1,       \
                                                embi_js_value *out)                                               \
    {                                                                                                             \
        return lst_replace(c, self, a0, a1, out);                                                                 \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_removeItem(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0,                 \
                                               embi_js_value *out)                                                \
    {                                                                                                             \
        return lst_remove(c, self, a0, out);                                                                      \
    }                                                                                                             \
    embi_dom_err embi_impl_##NAME##_appendItem(embi_bind_ctx *c, embi_bind_obj *self, void *a0, embi_js_value *out) \
    {                                                                                                             \
        return lst_append(c, self, a0, out);                                                                      \
    }

LIST_IFACE(SVGLengthList)
LIST_IFACE(SVGNumberList)
LIST_IFACE(SVGPointList)
LIST_IFACE(SVGTransformList)

embi_js_value embi_impl_SVGTransformList_createSVGTransformFromMatrix(embi_bind_ctx *c, embi_bind_obj *self,
                                                                       embi_dict_DOMMatrix2DInit a0)
{
    (void)self;
    return embi_svg_new_transform_from_init(c, &a0);
}

embi_js_value embi_impl_SVGTransformList_consolidate(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (plist(self)->ro) {
        (void)ro_list(c, self);
        return EMBI_JS_EXCEPTION;
    }
    uint32_t n = lst_len(c, self);
    if (n == 0) {
        return EMBI_JS_NULL;
    }
    /* acc = t0 * t1 * ... * t(n-1) (a 2D affine matrix a b c d e f). */
    double a[6] = {1, 0, 0, 1, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        const sv *t = item_at(c, self, i);
        const double *m = t->u.tf.m;
        double r[6] = {a[0] * m[0] + a[2] * m[1],        a[1] * m[0] + a[3] * m[1],
                       a[0] * m[2] + a[2] * m[3],        a[1] * m[2] + a[3] * m[3],
                       a[0] * m[4] + a[2] * m[5] + a[4], a[1] * m[4] + a[3] * m[5] + a[5]};
        SDL_memcpy(a, r, sizeof a);
    }
    embi_js_value v;
    sv *t = embi_sv_new(c, EMBI_IF_SVGTransform, SV_TF, &v);
    embi_bind_pin(c, v);
    t->u.tf.type = SVG_TF_MATRIX;
    SDL_memcpy(t->u.tf.m, a, sizeof a);
    lst_clear_items(c, self);
    t->bound = SB_ITEM;
    t->attr = plist(self)->attr;
    embi_bind_set_ref(t->self, embi_bind_obj_value(self));
    lst_insert(c, self, 0, v);
    embi_svg_list_commit(c, self);
    return v;
}

/* ------------------------------------------------------------------ SVGStringList */

typedef struct strs {
    embi_str *v;
    uint32_t n;
} strs;

static bool strs_commas(const sv *pl)
{
    return pl->attr == SA_systemLanguage;
}

static strs strs_parse(embi_bind_ctx *c, embi_bind_obj *L)
{
    sv *pl = plist(L);
    strs r = {nullptr, 0};
    embi_node *n = embi_bind_owner_node(L);
    embi_str t = {"", 0};
    if (n == nullptr || !embi_sv_attr_text(c, n, pl->attr, &t)) {
        return r;
    }
    r.v = (embi_str *)embi_bind_alloc(c, ((size_t)t.len / 2 + 2) * sizeof(embi_str));
    uint32_t p = 0;
    bool commas = strs_commas(pl);
    while (p < t.len) {
        while (p < t.len && (svg_ws(t.ptr[p]) || (commas && t.ptr[p] == ','))) {
            p++;
        }
        if (p >= t.len) {
            break;
        }
        uint32_t b = p;
        while (p < t.len && !svg_ws(t.ptr[p]) && !(commas && t.ptr[p] == ',')) {
            p++;
        }
        uint32_t e = p;
        if (commas) {
            while (e > b && svg_ws(t.ptr[e - 1])) {
                e--;
            }
        }
        r.v[r.n++] = (embi_str){t.ptr + b, e - b};
    }
    return r;
}

static void strs_write(embi_bind_ctx *c, embi_bind_obj *L, const strs *s)
{
    sv *pl = plist(L);
    embi_sb sb = {0};
    for (uint32_t i = 0; i < s->n; i++) {
        if (i > 0) {
            embi_sb_append(&sb, strs_commas(pl) ? EMBI_STR(", ") : EMBI_STR(" "));
        }
        embi_sb_append(&sb, s->v[i]);
    }
    embi_node *n = embi_bind_owner_node(L);
    if (n != nullptr) {
        embi_sv_set_attr(c, n, pl->attr, (embi_str){sb.data ? sb.data : "", sb.len});
    }
    embi_sb_free(&sb);
}

uint32_t embi_impl_SVGStringList_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    return strs_parse(c, self).n;
}

uint32_t embi_impl_SVGStringList_get_numberOfItems(embi_bind_ctx *c, embi_bind_obj *self)
{
    return strs_parse(c, self).n;
}

embi_js_value embi_impl_SVGStringList_indexed(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    strs s = strs_parse(c, self);
    if (a0 >= s.n) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_utf8_value(c, s.v[a0]);
}

embi_dom_err embi_impl_SVGStringList_clear(embi_bind_ctx *c, embi_bind_obj *self)
{
    strs s = {nullptr, 0};
    strs_write(c, self, &s);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGStringList_initialize(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr *out)
{
    embi_str one = embi_bind_utf8(c, a0);
    strs s = {&one, 1};
    strs_write(c, self, &s);
    *out = a0;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGStringList_getItem(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0, embi_dstr *out)
{
    strs s = strs_parse(c, self);
    if (a0 >= s.n) {
        return index_error(c);
    }
    *out = embi_bind_tmp_utf8(c, s.v[a0]);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGStringList_insertItemBefore(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, uint32_t a1,
                                                      embi_dstr *out)
{
    strs s = strs_parse(c, self);
    embi_str v = embi_bind_utf8(c, a0);
    embi_str *arr = (embi_str *)embi_bind_alloc(c, ((size_t)s.n + 1) * sizeof(embi_str));
    uint32_t at = a1 > s.n ? s.n : a1;
    for (uint32_t i = 0, j = 0; i <= s.n; i++) {
        arr[i] = (i == at) ? v : s.v[j++];
    }
    strs w = {arr, s.n + 1};
    strs_write(c, self, &w);
    *out = a0;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGStringList_replaceItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, uint32_t a1,
                                                 embi_dstr *out)
{
    strs s = strs_parse(c, self);
    if (a1 >= s.n) {
        return index_error(c);
    }
    s.v[a1] = embi_bind_utf8(c, a0);
    strs_write(c, self, &s);
    *out = a0;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGStringList_removeItem(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0, embi_dstr *out)
{
    strs s = strs_parse(c, self);
    if (a0 >= s.n) {
        return index_error(c);
    }
    *out = embi_bind_tmp_utf8(c, s.v[a0]);
    for (uint32_t i = a0; i + 1 < s.n; i++) {
        s.v[i] = s.v[i + 1];
    }
    s.n--;
    strs_write(c, self, &s);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGStringList_appendItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_dstr *out)
{
    strs s = strs_parse(c, self);
    embi_str *arr = (embi_str *)embi_bind_alloc(c, ((size_t)s.n + 1) * sizeof(embi_str));
    for (uint32_t i = 0; i < s.n; i++) {
        arr[i] = s.v[i];
    }
    arr[s.n] = embi_bind_utf8(c, a0);
    strs w = {arr, s.n + 1};
    strs_write(c, self, &w);
    *out = a0;
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ the indexed property setter */

/* list[index] = value (WebIDL 3.9.3, the setter of the SVG 2 list interfaces): the value is converted to the item type
 * (a TypeError otherwise) and replaces the item, replaceItem(value, index): the item must exist (IndexSizeError). The
 * descriptor must be a plain data one: an accessor can not be defined on an index. */
static int indexed_set(embi_js_object *o, uint32_t index, const embi_js_pdesc *d, uint16_t item_iface)
{
    embi_bind_obj *bo = o->internal;
    if (!bo || !bo->ctx || embi_bind_stale(bo)) {
        return EMBI_JS_NO;
    }
    if ((d->has & (EMBI_JS_PD_GET | EMBI_JS_PD_SET)) || !(d->has & EMBI_JS_PD_VALUE)) {
        return EMBI_JS_NO;
    }
    embi_bind_ctx *c = bo->ctx;
    embi_bind_scope sc = embi_bind_enter(c);
    int result = EMBI_JS_YES;
    embi_js_value out;
    if (item_iface == EMBI_IF_NONE) {
        embi_dstr s;
        if (!embi_bind_to_dstr(c, d->value, &s) || embi_impl_SVGStringList_replaceItem(c, bo, s, index, &s) != EMBI_DOM_OK) {
            result = EMBI_JS_THREW;
        }
    } else {
        void *item = nullptr;
        if (!embi_bind_conv_other(c, d->value, item_iface, false, "The value", &item) ||
            lst_replace(c, bo, item, index, &out) != EMBI_DOM_OK) {
            result = EMBI_JS_THREW;
        }
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return result;
}

int embi_impl_SVGLengthList_indexed_set(embi_js_runtime *rt, embi_js_object *o, uint32_t index, const embi_js_pdesc *d)
{
    (void)rt;
    return indexed_set(o, index, d, EMBI_IF_SVGLength);
}

int embi_impl_SVGNumberList_indexed_set(embi_js_runtime *rt, embi_js_object *o, uint32_t index, const embi_js_pdesc *d)
{
    (void)rt;
    return indexed_set(o, index, d, EMBI_IF_SVGNumber);
}

int embi_impl_SVGPointList_indexed_set(embi_js_runtime *rt, embi_js_object *o, uint32_t index, const embi_js_pdesc *d)
{
    (void)rt;
    return indexed_set(o, index, d, EMBI_IF_DOMPoint);
}

int embi_impl_SVGTransformList_indexed_set(embi_js_runtime *rt, embi_js_object *o, uint32_t index, const embi_js_pdesc *d)
{
    (void)rt;
    return indexed_set(o, index, d, EMBI_IF_SVGTransform);
}

int embi_impl_SVGStringList_indexed_set(embi_js_runtime *rt, embi_js_object *o, uint32_t index, const embi_js_pdesc *d)
{
    (void)rt;
    return indexed_set(o, index, d, EMBI_IF_NONE);
}

/* ------------------------------------------------------------------ animated wrappers */

embi_js_value embi_svg_anim_view(embi_bind_ctx *c, embi_node *self, uint16_t iface, uint32_t idx)
{
    embi_bind_obj *owner = embi_impl_node_rec(c, self);
    return embi_bind_view_new(c, iface, owner, idx, nullptr, 0);
}

/* An object of the animated value, bound to the attribute of the element. */
static embi_js_value anim_value(embi_bind_ctx *c, embi_bind_obj *self, uint16_t iface, uint8_t kind, bool ro)
{
    embi_js_value v;
    sv *p = embi_sv_new(c, iface, kind, &v);
    p->bound = SB_ATTR;
    p->attr = (uint16_t)self->aux;
    p->ro = ro;
    if (self->owner != nullptr) {
        embi_bind_set_ref(p->self, embi_bind_obj_value(self->owner));
    }
    embi_sv_load(c, p);
    return v;
}

#define ANIM_VALUE_PAIR(ANIM, IFACE, KIND)                                                               \
    embi_js_value embi_impl_##ANIM##_get_baseVal(embi_bind_ctx *c, embi_bind_obj *self)                    \
    {                                                                                                    \
        return anim_value(c, self, EMBI_IF_##IFACE, KIND, false);                                        \
    }                                                                                                    \
    embi_js_value embi_impl_##ANIM##_get_animVal(embi_bind_ctx *c, embi_bind_obj *self)                    \
    {                                                                                                    \
        return anim_value(c, self, EMBI_IF_##IFACE, KIND, true);                                         \
    }

ANIM_VALUE_PAIR(SVGAnimatedLength, SVGLength, SV_LEN)
ANIM_VALUE_PAIR(SVGAnimatedAngle, SVGAngle, SV_ANGLE)
ANIM_VALUE_PAIR(SVGAnimatedRect, SVGRect, SV_RECT)
ANIM_VALUE_PAIR(SVGAnimatedPreserveAspectRatio, SVGPreserveAspectRatio, SV_PAR)

/* The lists of an animated list: baseVal and animVal are distinct objects (SameObject-cached by the generated getter);
 * animVal is read only and shows the base value (no SMIL). */
static embi_js_value anim_list(embi_bind_ctx *c, embi_bind_obj *self, uint16_t iface, bool ro)
{
    embi_node *n = embi_bind_owner_node(self);
    if (n == nullptr) {
        return EMBI_JS_NULL;
    }
    return embi_svg_list_view(c, n, iface, self->aux, ro);
}

#define ANIM_LIST_PAIR(ANIM, IFACE)                                                                      \
    embi_js_value embi_impl_##ANIM##_get_baseVal(embi_bind_ctx *c, embi_bind_obj *self)                    \
    {                                                                                                    \
        return anim_list(c, self, EMBI_IF_##IFACE, false);                                               \
    }                                                                                                    \
    embi_js_value embi_impl_##ANIM##_get_animVal(embi_bind_ctx *c, embi_bind_obj *self)                    \
    {                                                                                                    \
        return anim_list(c, self, EMBI_IF_##IFACE, true);                                                \
    }

ANIM_LIST_PAIR(SVGAnimatedLengthList, SVGLengthList)
ANIM_LIST_PAIR(SVGAnimatedNumberList, SVGNumberList)
ANIM_LIST_PAIR(SVGAnimatedTransformList, SVGTransformList)

/* SVGAnimatedString: the attribute text. */
embi_dstr embi_impl_SVGAnimatedString_get_baseVal(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_node *n = embi_bind_owner_node(self);
    embi_str v;
    if (n && embi_sv_attr_text(c, n, self->aux, &v)) {
        return embi_bind_tmp_utf8(c, v);
    }
    return embi_impl_lit("");
}

void embi_impl_SVGAnimatedString_set_baseVal(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)c;
    embi_node *n = embi_bind_owner_node(self);
    if (n) {
        (void)embi_el_set_attribute((embi_element *)n, cstr(embi_sv_attr_name(self->aux)), a0);
    }
}

embi_dstr embi_impl_SVGAnimatedString_get_animVal(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_SVGAnimatedString_get_baseVal(c, self);
}

/* SVGAnimatedNumber: a plain number (a percentage counts out of 100, as the offset of a stop does). */
embi_dom_err embi_impl_SVGAnimatedNumber_get_baseVal(embi_bind_ctx *c, embi_bind_obj *self, double *out)
{
    embi_node *n = embi_bind_owner_node(self);
    embi_str t = {"", 0};
    double v = 0;
    if (n && embi_sv_attr_text(c, n, self->aux, &t)) {
        uint32_t p = 0;
        while (p < t.len && svg_ws(t.ptr[p])) {
            p++;
        }
        bool neg = false;
        if (p < t.len && (t.ptr[p] == '-' || t.ptr[p] == '+')) {
            neg = t.ptr[p] == '-';
            p++;
        }
        size_t used = 0;
        if (p < t.len && ((t.ptr[p] >= '0' && t.ptr[p] <= '9') || t.ptr[p] == '.')) {
            double d = embi_num_parse_decimal(t.ptr + p, t.len - p, &used);
            if (used > 0 && d - d == 0.0) {
                p += (uint32_t)used;
                bool pct = p < t.len && t.ptr[p] == '%';
                if (pct) {
                    p++;
                }
                while (p < t.len && svg_ws(t.ptr[p])) {
                    p++;
                }
                if (p == t.len) {
                    v = (neg ? -d : d) / (pct ? 100.0 : 1.0);
                }
            }
        }
    }
    *out = v;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAnimatedNumber_set_baseVal(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    if (embi_sv_float_range_error(c, a0)) {
        return EMBI_BIND_THREW;
    }
    embi_node *n = embi_bind_owner_node(self);
    if (n) {
        char buf[64];
        size_t k = embi_num_to_str(a0, buf);
        embi_sv_set_attr(c, n, self->aux, (embi_str){buf, (uint32_t)k});
    }
    return EMBI_DOM_OK;
}

double embi_impl_SVGAnimatedNumber_get_animVal(embi_bind_ctx *c, embi_bind_obj *self)
{
    double v = 0;
    (void)embi_impl_SVGAnimatedNumber_get_baseVal(c, self, &v);
    return v;
}

/* ------------------------------------------------------------------ SVGAnimatedEnumeration */

typedef struct enum_def {
    uint16_t attr;
    uint8_t count;
    uint8_t def;
    const char *kw[3];
} enum_def;

static const enum_def enum_defs[] = {
    {SA_clipPathUnits, 2, 1, {"userSpaceOnUse", "objectBoundingBox", nullptr}},
    {SA_gradientUnits, 2, 2, {"userSpaceOnUse", "objectBoundingBox", nullptr}},
    {SA_spreadMethod, 3, 1, {"pad", "reflect", "repeat"}},
    {SA_markerUnits, 2, 2, {"userSpaceOnUse", "strokeWidth", nullptr}},
    {SA_maskUnits, 2, 2, {"userSpaceOnUse", "objectBoundingBox", nullptr}},
    {SA_maskContentUnits, 2, 1, {"userSpaceOnUse", "objectBoundingBox", nullptr}},
    {SA_patternUnits, 2, 2, {"userSpaceOnUse", "objectBoundingBox", nullptr}},
    {SA_patternContentUnits, 2, 1, {"userSpaceOnUse", "objectBoundingBox", nullptr}},
    {SA_lengthAdjust, 2, 1, {"spacing", "spacingAndGlyphs", nullptr}},
    {SA_method, 2, 1, {"align", "stretch", nullptr}},
    {SA_spacing, 2, 2, {"auto", "exact", nullptr}},
    {SA_side, 2, 1, {"left", "right", nullptr}},
};

static const enum_def *enum_def_of(uint32_t attr)
{
    for (uint32_t i = 0; i < sizeof enum_defs / sizeof enum_defs[0]; i++) {
        if (enum_defs[i].attr == attr) {
            return &enum_defs[i];
        }
    }
    return nullptr;
}

embi_dom_err embi_impl_SVGAnimatedEnumeration_get_baseVal(embi_bind_ctx *c, embi_bind_obj *self, uint16_t *out)
{
    embi_node *n = embi_bind_owner_node(self);
    embi_str t = {"", 0};
    bool has = n && embi_sv_attr_text(c, n, self->aux, &t);
    if (self->aux == SA_orientType) {
        /* orient="auto" is 1, "auto-start-reverse" 3, an angle (or none) 2 */
        *out = (has && embi_str_eq(t, EMBI_STR("auto"))) ? 1 : (has && embi_str_eq(t, EMBI_STR("auto-start-reverse"))) ? 3 : 2;
        return EMBI_DOM_OK;
    }
    const enum_def *d = enum_def_of(self->aux);
    if (d == nullptr) {
        *out = 0;
        return EMBI_DOM_OK;
    }
    if (!has) {
        *out = d->def;
        return EMBI_DOM_OK;
    }
    *out = d->def; /* an invalid keyword reverts to the initial value */
    for (uint32_t i = 0; i < d->count; i++) {
        if (embi_str_eq(t, cstr(d->kw[i]))) {
            *out = (uint16_t)(i + 1);
        }
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAnimatedEnumeration_set_baseVal(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0)
{
    embi_node *n = embi_bind_owner_node(self);
    if (self->aux == SA_orientType) {
        if (a0 < 1 || a0 > 3) {
            (void)embi_js_throw_type_error(embi_bind_runtime(c), "The orientType value is not valid.");
            return EMBI_BIND_THREW;
        }
        if (n && a0 == 2) {
            /* an angle: keep the numeric orient it has, otherwise 0 */
            embi_str t = {"", 0};
            if (!embi_sv_attr_text(c, n, SA_orient, &t) || embi_str_eq(t, EMBI_STR("auto")) ||
                embi_str_eq(t, EMBI_STR("auto-start-reverse"))) {
                embi_sv_set_attr(c, n, SA_orient, EMBI_STR("0"));
            }
        } else if (n) {
            embi_sv_set_attr(c, n, SA_orient, a0 == 1 ? EMBI_STR("auto") : EMBI_STR("auto-start-reverse"));
        }
        return EMBI_DOM_OK;
    }
    const enum_def *d = enum_def_of(self->aux);
    if (d == nullptr || a0 < 1 || a0 > d->count) {
        (void)embi_js_throw_type_error(embi_bind_runtime(c), "The enumeration value is not valid.");
        return EMBI_BIND_THREW;
    }
    if (n) {
        embi_sv_set_attr(c, n, self->aux, cstr(d->kw[a0 - 1]));
    }
    return EMBI_DOM_OK;
}

uint16_t embi_impl_SVGAnimatedEnumeration_get_animVal(embi_bind_ctx *c, embi_bind_obj *self)
{
    uint16_t v = 0;
    (void)embi_impl_SVGAnimatedEnumeration_get_baseVal(c, self, &v);
    return v;
}
