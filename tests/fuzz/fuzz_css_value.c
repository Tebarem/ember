// fuzz-modules: css dom loader
/* CSS value parsing for every property in props.txt, plus colors, calc(), registered custom
 * property syntax, var() substitution and the typed-value parser. Input: byte 0 = property
 * (low 8 bits), byte 1: bit 0 = property bit 8, bits 1..3 = mode; rest = value text.
 * Modes: 0 declaration value -> longhands -> serialize (the property index wraps over the table),
 * 1 expand_shorthand + parse_longhand + initial value, 2 colors (parse, serialize, convert to sRGB),
 * 3 math functions (parse, evaluate with a length context, reduce), 4 registered syntax (a NUL
 * splits the syntax from the value), 5 var() substitution (a NUL splits the value from the variable
 * definition) and parse after substitution, 6 typed grammar parser with the type mask from byte 2,
 * 7 shorthand/longhand, logical <-> physical property mapping. */
#define FUZZ_RESET_HOOKS() embi_sel_lower_cache_reset();
#include "css/props.h"
#include "css/selector.h"
#include "fuzz_common.h"

typedef struct vars {
    embi_css_slice def;
} vars;

static bool lookup_cb(void *user, embi_str name, embi_css_slice *value)
{
    vars *v = user;
    if (name.len == 0 || (name.len & 1)) {
        return false;
    }
    *value = v->def;
    return true;
}

static void ser(const embi_longhand *lh, uint32_t n)
{
    embi_sb sb = {0};
    for (uint32_t i = 0; i < n; i++) {
        embi_value_serialize(&sb, &lh[i].value);
    }
    embi_sb_free(&sb);
}

static void do_calc(embi_arena *a, embi_css_slice cvs)
{
    embi_len_ctx lc = {16, 16, 800, 600, 400, 300, {8, 11, 8, 16, 19}, {8, 11, 8, 16, 19}};
    embi_calc_env env = {123.0, embi_len_px_cb, &lc};
    for (uint32_t i = 0; i < cvs.n; i++) {
        const embi_calc *c = nullptr;
        uint8_t type = 0;
        if (cvs.p[i].tok.type == EMBI_CSS_FUNCTION && embi_calc_parse_cv(a, &cvs.p[i], &c, &type)) {
            (void)embi_calc_eval(c, &env);
            double v = 0;
            if (embi_calc_is_constant(c)) {
                (void)embi_calc_reduce(a, c, &v);
            }
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t b0 = fuzz_u8(&in);
    uint8_t b1 = fuzz_u8(&in);
    unsigned mode = (b1 >> 1) & 7;
    unsigned raw = (unsigned)b0 | ((unsigned)(b1 & 1) << 8);
    embi_prop_id prop = (embi_prop_id)(1 + raw % (EMBI_PROP_COUNT - 1));
    uint8_t b2 = (mode == 6) ? fuzz_u8(&in) : 0;

    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    const uint8_t *nul = in.n ? memchr(in.p, 0, in.n) : nullptr;
    embi_str first = {in.n ? (const char *)in.p : "", (uint32_t)(nul ? (size_t)(nul - in.p) : in.n)};
    embi_str second = {"", 0};
    if (nul) {
        second = (embi_str){(const char *)nul + 1, (uint32_t)(in.n - (size_t)(nul - in.p) - 1)};
    }
    embi_css_slice cvs = embi_css_parse_cvs(&a, first);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    switch (mode) {
    case 0: {
        uint32_t n = embi_css_parse_decl(&a, prop, cvs, lh, EMBI_LONGHAND_MAX);
        ser(lh, n);
        (void)embi_css_has_var(cvs);
        (void)embi_css_value_opaque_ok(cvs);
        break;
    }
    case 1: {
        if (embi_prop_is_shorthand(prop)) {
            uint32_t n = embi_css_expand_shorthand(&a, prop, cvs, lh, EMBI_LONGHAND_MAX);
            ser(lh, n);
        } else {
            embi_value v;
            if (embi_value_parse_longhand(&a, prop, cvs, &v)) {
                embi_sb sb = {0};
                embi_value_serialize(&sb, &v);
                embi_sb_free(&sb);
            }
            if (cvs.n) {
                (void)embi_value_parse_one(&a, prop, &cvs.p[0], &v);
            }
            (void)embi_value_initial(&a, prop, &v);
        }
        break;
    }
    case 2: {
        embi_color c;
        if (embi_color_parse(cvs, &c)) {
            embi_sb sb = {0};
            embi_color_serialize(&sb, &c);
            embi_sb_free(&sb);
            embi_color s;
            (void)embi_color_to_srgb(&c, &s);
            uint8_t px[4];
            (void)embi_color_to_srgb8(&c, px);
        }
        embi_color nc;
        (void)embi_color_named(first, &nc);
        uint16_t idx;
        (void)embi_color_system(first, &idx);
        break;
    }
    case 3:
        do_calc(&a, cvs);
        (void)embi_css_parse_decl(&a, prop, cvs, lh, EMBI_LONGHAND_MAX);
        break;
    case 4: {
        embi_css_slice val = embi_css_parse_cvs(&a, second);
        embi_value v;
        if (embi_css_registered_parse(&a, first, val, &v)) {
            embi_sb sb = {0};
            embi_value_serialize(&sb, &v);
            embi_sb_free(&sb);
        }
        break;
    }
    case 5: {
        vars vs = {embi_css_parse_cvs(&a, second)};
        embi_var_env env = {.user = &vs, .lookup = lookup_cb, .max_tokens = 4096};
        embi_css_slice out = {nullptr, 0};
        (void)embi_css_substitute_vars(&a, &env, EMBI_STR("--self"), cvs, &out);
        uint32_t n = embi_css_parse_decl_vars(&a, prop, cvs, &env, lh, EMBI_LONGHAND_MAX);
        ser(lh, n);
        break;
    }
    case 6: {
        uint32_t types = ((uint32_t)b2 << 6) ^ (uint32_t)b2;
        embi_value v;
        for (uint32_t i = 0; i < cvs.n && i < 4; i++) {
            (void)embi_value_parse_types(&a, types & 0x7FFFu, &cvs.p[i], &v);
        }
        break;
    }
    default: {
        (void)embi_prop_is_logical(prop);
        static const embi_str wms[] = {EMBI_STR("horizontal-tb"), EMBI_STR("vertical-rl"),
                                       EMBI_STR("vertical-lr"), EMBI_STR("sideways-rl"),
                                       EMBI_STR("sideways-lr")};
        static const embi_str dirs[] = {EMBI_STR("ltr"), EMBI_STR("rtl")};
        uint16_t wm = embi_kw_lookup(wms[b0 % 5]);
        uint16_t dir = embi_kw_lookup(dirs[(b0 >> 3) & 1]);
        embi_prop_id ph = embi_prop_to_physical(prop, wm, dir);
        (void)embi_prop_to_logical(ph, wm, dir);
        (void)embi_prop_longhands(prop);
        (void)embi_prop_lookup(first);
        (void)embi_kw_lookup(first);
        uint32_t n = embi_css_parse_decl(&a, embi_prop_lookup(first) ? embi_prop_lookup(first) : prop,
                                         embi_css_parse_cvs(&a, second), lh, EMBI_LONGHAND_MAX);
        ser(lh, n);
        break;
    }
    }
    embi_arena_free(&a);
    FUZZ_GUARD_END();
    return 0;
}
