/*
 * Implementations of the Test* interfaces of testing.webidl. They exercise every feature of the
 * generator (conversions, overloads, unions, sequences, records, promises, iterables) and are
 * only exposed by embi_bind_expose_testing.
 */
#include "bindings/impl.h"
#include "bindings/gen/testing.h"

enum { TXT = 96 };

typedef struct widget {
    int32_t count;
    uint8_t small;
    int16_t sclamp;
    int64_t lclamp;
    uint64_t big;
    uint64_t wrapped;
    int32_t mode; /* -1: null */
    bool note_null;
    char note[TXT];
    char text[TXT];
} widget;

static embi_dstr cstr(embi_bind_ctx *c, const char *s)
{
    return embi_bind_tmp_utf8(c, (embi_str){s, (uint32_t)SDL_strlen(s)});
}

static void copy_into(embi_bind_ctx *c, char *dst, embi_dstr d)
{
    embi_str u = embi_bind_utf8(c, d);
    uint32_t n = u.len < TXT - 1 ? u.len : TXT - 1;
    if (n) {
        SDL_memcpy(dst, u.ptr, n);
    }
    dst[n] = '\0';
}

static widget *wid(embi_bind_obj *self)
{
    return self->ptr;
}

static embi_dstr fmt(embi_bind_ctx *c, const char *f, ...) SDL_PRINTF_VARARG_FUNC(2);
static embi_dstr fmt(embi_bind_ctx *c, const char *f, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, f);
    SDL_vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return cstr(c, buf);
}

/* ------------------------------------------------------------------ TestBase */

embi_js_value embi_impl_TestBase_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_TestBase, c, nullptr, 0);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

embi_dstr embi_impl_TestBase_get_baseName(embi_bind_ctx *c, embi_bind_obj *self)
{
    return cstr(c, self->iface == EMBI_IF_TestWidget ? "widget" : "base");
}

embi_dstr embi_impl_TestBase_describe(embi_bind_ctx *c, embi_bind_obj *self)
{
    return cstr(c, self->iface == EMBI_IF_TestWidget ? "TestWidget" : "TestBase");
}

/* ------------------------------------------------------------------ TestWidget */

embi_js_value embi_impl_TestWidget_constructor(embi_bind_ctx *c, embi_js_value new_target,
                                               embi_dict_TestOptions a0)
{
    widget *w = embi_calloc(1, sizeof *w);
    w->count = a0.count;
    w->mode = -1;
    w->note_null = true;
    copy_into(c, w->text, a0.label);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_TestWidget, w, w, sizeof *w);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

int32_t embi_impl_TestWidget_get_count(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->count;
}

void embi_impl_TestWidget_set_count(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)c;
    wid(self)->count = a0;
}

uint8_t embi_impl_TestWidget_get_small(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->small;
}

void embi_impl_TestWidget_set_small(embi_bind_ctx *c, embi_bind_obj *self, uint8_t a0)
{
    (void)c;
    wid(self)->small = a0;
}

int16_t embi_impl_TestWidget_get_sclamp(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->sclamp;
}

void embi_impl_TestWidget_set_sclamp(embi_bind_ctx *c, embi_bind_obj *self, int16_t a0)
{
    (void)c;
    wid(self)->sclamp = a0;
}

int64_t embi_impl_TestWidget_get_lclamp(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->lclamp;
}

void embi_impl_TestWidget_set_lclamp(embi_bind_ctx *c, embi_bind_obj *self, int64_t a0)
{
    (void)c;
    wid(self)->lclamp = a0;
}

uint64_t embi_impl_TestWidget_get_big(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->big;
}

void embi_impl_TestWidget_set_big(embi_bind_ctx *c, embi_bind_obj *self, uint64_t a0)
{
    (void)c;
    wid(self)->big = a0;
}

uint64_t embi_impl_TestWidget_get_wrapped(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->wrapped;
}

void embi_impl_TestWidget_set_wrapped(embi_bind_ctx *c, embi_bind_obj *self, uint64_t a0)
{
    (void)c;
    wid(self)->wrapped = a0;
}

int32_t embi_impl_TestWidget_get_mode(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return wid(self)->mode;
}

void embi_impl_TestWidget_set_mode(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)c;
    wid(self)->mode = a0;
}

embi_js_value embi_impl_TestWidget_get_note(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (wid(self)->note_null) {
        return EMBI_JS_NULL;
    }
    return embi_bind_dstr_value(c, cstr(c, wid(self)->note));
}

void embi_impl_TestWidget_set_note(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_nstr a0)
{
    wid(self)->note_null = a0.is_null;
    if (!a0.is_null) {
        copy_into(c, wid(self)->note, a0.s);
    }
}

embi_dstr embi_impl_TestWidget_get_text(embi_bind_ctx *c, embi_bind_obj *self)
{
    return cstr(c, wid(self)->text);
}

void embi_impl_TestWidget_set_text(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    copy_into(c, wid(self)->text, a0);
}

embi_dstr embi_impl_TestWidget_get_constant(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return cstr(c, "constant");
}

embi_js_value embi_impl_TestWidget_get_same(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return embi_impl_TestBase_constructor(c, EMBI_JS_UNDEFINED);
}

embi_js_value embi_impl_TestWidget_get_self(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_bind_obj_value(self);
}

int32_t embi_impl_TestWidget_get_replaceable(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return 11;
}

int32_t embi_impl_TestWidget_get_forged(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return 13;
}

embi_dict_TestResult embi_impl_TestWidget_result(embi_bind_ctx *c, embi_bind_obj *self,
                                                 embi_dict_TestOptions a0)
{
    (void)self;
    embi_dict_TestResult r = {0};
    r.label = a0.label;
    r.count = a0.count;
    r.flag = a0.flag;
    int32_t *nums = (int32_t *)embi_bind_alloc(c, 3 * sizeof(int32_t));
    nums[0] = a0.count;
    nums[1] = a0.count * 2;
    nums[2] = (int32_t)(a0.ratio * 100);
    r.numbers = (embi_bind_seq){nums, 3};
    return r;
}

embi_bind_seq embi_impl_TestWidget_sequenceOf(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_seq a0)
{
    (void)self;
    int32_t *out = (int32_t *)embi_bind_alloc(c, (a0.len ? a0.len : 1) * sizeof(int32_t));
    const int32_t *in = a0.data;
    for (uint32_t i = 0; i < a0.len; i++) {
        out[i] = in[i] + 1;
    }
    return (embi_bind_seq){out, a0.len};
}

embi_bind_seq embi_impl_TestWidget_names(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    embi_dstr *out = (embi_dstr *)embi_bind_alloc(c, 3 * sizeof(embi_dstr));
    out[0] = cstr(c, "a");
    out[1] = cstr(c, "bb");
    out[2] = cstr(c, "ccc");
    return (embi_bind_seq){out, 3};
}

embi_bind_rec embi_impl_TestWidget_recordOf(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_rec a0)
{
    (void)c;
    (void)self;
    int32_t *v = a0.vals;
    for (uint32_t i = 0; i < a0.len; i++) {
        v[i] *= 2;
    }
    return a0;
}

embi_js_value embi_impl_TestWidget_promised(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    return embi_bind_promise_resolved(c, embi_js_from_int32(a0 + 1));
}

embi_js_value embi_impl_TestWidget_promisedFail(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    embi_js_value dom = embi_bind_dom_exception(c, 0, cstr(c, "TestError"), fmt(c, "failed %d", (int)a0));
    return embi_bind_promise_rejected(c, dom);
}

embi_js_value embi_impl_TestWidget_union(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_union a0)
{
    (void)self;
    switch (a0.kind) {
    case 1:
        return embi_bind_dstr_value(c, a0.u.str);
    case 2:
        return embi_js_from_double(a0.u.num);
    default:
        return a0.u.b ? embi_bind_utf8_value(c, EMBI_STR("yes")) : embi_bind_utf8_value(c, EMBI_STR("no"));
    }
}

embi_dstr embi_impl_TestWidget_kind(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_union a0)
{
    (void)self;
    switch (a0.kind) {
    case 1:
        return cstr(c, "node");
    case 2:
        return fmt(c, "string");
    case 3:
        return cstr(c, "number");
    case 4:
        return cstr(c, "sequence");
    case 5:
        return fmt(c, "dict:%d", (int)((const embi_dict_TestOptions *)a0.u.ptr)->count);
    default:
        return cstr(c, "none");
    }
}

embi_dstr embi_impl_TestWidget_overload_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    return fmt(c, "string:%.*s", (int)embi_bind_utf8(c, a0).len, (const char *)embi_bind_utf8(c, a0).ptr);
}

embi_dstr embi_impl_TestWidget_overload_1(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0)
{
    (void)self;
    return fmt(c, "long:%d", (int)a0);
}

embi_dstr embi_impl_TestWidget_overload_2(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0)
{
    (void)self;
    (void)a0;
    return cstr(c, "node");
}

embi_dstr embi_impl_TestWidget_overload_3(embi_bind_ctx *c, embi_bind_obj *self, bool a0, embi_dstr a1)
{
    (void)self;
    embi_str t = embi_bind_utf8(c, a1);
    return fmt(c, "bool-string:%d:%.*s", a0 ? 1 : 0, (int)t.len, (const char *)t.ptr);
}

embi_dstr embi_impl_TestWidget_overload_4(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    (void)self;
    embi_js_value arg = embi_js_from_int32(5);
    embi_js_value r = embi_js_call(c->rt, a0, EMBI_JS_UNDEFINED, 1, &arg);
    if (r == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return cstr(c, "callback-threw");
    }
    return cstr(c, "callback");
}

embi_dstr embi_impl_TestWidget_variadic(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, int a1n,
                                        const int32_t *a1p)
{
    (void)self;
    int64_t sum = 0;
    for (int i = 0; i < a1n; i++) {
        sum += a1p[i];
    }
    embi_str t = embi_bind_utf8(c, a0);
    return fmt(c, "%.*s:%d:%d", (int)t.len, (const char *)t.ptr, a1n, (int)sum);
}

embi_dom_err embi_impl_TestWidget_fail(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0)
{
    (void)self;
    if (a0 <= EMBI_DOM_ERR_NOT_SUPPORTED) {
        return (embi_dom_err)a0;
    }
    embi_bind_throw_named(c, "TestError", "custom failure");
    return EMBI_BIND_THREW;
}

int32_t embi_impl_TestWidget_sum(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_seq a0)
{
    (void)c;
    (void)self;
    const int32_t *v = a0.data;
    int32_t s = 0;
    for (uint32_t i = 0; i < a0.len; i++) {
        s += v[i];
    }
    return s;
}

embi_dstr embi_impl_TestWidget_make(embi_bind_ctx *c)
{
    return cstr(c, "made");
}

uint32_t embi_impl_TestWidget_iter_size(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (uint32_t)(wid(self)->count < 0 ? 0 : wid(self)->count);
}

embi_js_value embi_impl_TestWidget_iter_key(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    (void)c;
    (void)self;
    return embi_js_from_int32((int32_t)i);
}

embi_js_value embi_impl_TestWidget_iter_value(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    (void)self;
    return embi_bind_dstr_value(c, fmt(c, "v%u", (unsigned)i));
}

embi_dstr embi_impl_TestWidget_stringifier(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fmt(c, "[TestWidget %d]", (int)wid(self)->count);
}

/* ------------------------------------------------------------------ TestList */

enum { LIST_N = 4 };

uint32_t embi_impl_TestList_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return LIST_N;
}

embi_js_value embi_impl_TestList_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    (void)self;
    if (a0 >= LIST_N) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_dstr_value(c, fmt(c, "item%u", (unsigned)a0));
}
