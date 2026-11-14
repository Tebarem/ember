// fuzz-modules: js jslex num base
/* JavaScript lexer, parser and bytecode compiler. Nothing runs. Input: byte 0 = mode, rest =
 * source text. Bit 0 = strict, bit 1 = also compile it as a Function-constructor unit
 * (embi_js_compile_ex with dynfn and embi_js_compile_function_only), bit 2 = also drive the lexer
 * directly (with regex and template rescans, like the parser does), bit 3 = skip the compiler
 * (lexer only). A runtime and realm are created for every input, so a finding never depends on
 * what an earlier input left behind. */
#include "fuzz_common.h"

#include "js/compiler.h"
#include "js/js.h"
#include "jslex/jslex.h"

static void drive_lexer(const uint8_t *p, size_t n, uint32_t flags)
{
    static uint16_t scratch[4096];
    embi_jslex lx;
    embi_jslex_init(&lx, p, (uint32_t)n, scratch, 4096, flags);
    embi_jstok t;
    unsigned guard = 0;
    uint32_t depth_tpl = 0;
    for (;;) {
        lx.regex_ok = (guard & 1) != 0;
        embi_jslex_next(&lx, &t);
        if (t.kind == EMBI_JST_EOF || t.kind == EMBI_JST_ERROR || ++guard > 200000) {
            break;
        }
        if ((t.kind == EMBI_JST_DIV || t.kind == EMBI_JST_DIV_EQ) && (guard % 3) == 0) {
            embi_jslex_rescan_regex(&lx, &t);
        } else if (t.kind == EMBI_JST_RBRACE && depth_tpl > 0 && (guard % 2) == 0) {
            embi_jslex_rescan_template(&lx, &t);
            depth_tpl--;
        } else if (t.kind == EMBI_JST_TPL_HEAD || t.kind == EMBI_JST_TPL_MIDDLE) {
            depth_tpl++;
        }
        if (t.kind == EMBI_JST_BIGINT) {
            char digits[64];
            (void)embi_jslex_bigint_digits(&lx, &t, digits, sizeof digits);
        }
        if (t.kind == EMBI_JST_TPL_HEAD || t.kind == EMBI_JST_TPL_MIDDLE || t.kind == EMBI_JST_TPL_TAIL ||
            t.kind == EMBI_JST_TPL_NOSUB) {
            uint16_t raw[64];
            (void)embi_jslex_raw16(&lx, &t, raw, 64);
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    bool strict = mode & 1;
    if (mode & 4) {
        drive_lexer(in.p, in.n, 0);
    }
    if (!(mode & 8)) {
        embi_js_runtime *rt = embi_js_runtime_new();
        embi_js_realm *r = embi_js_realm_new(rt);
        embi_js_value v = embi_js_compile(r, in.p, (uint32_t)in.n, "fuzz.js", strict);
        if (v == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
        }
        if (mode & 2) {
            v = embi_js_compile_ex(r, in.p, (uint32_t)in.n, "fuzz.js", strict, true);
            if (v == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(rt);
            }
            v = embi_js_compile_function_only(r, in.p, (uint32_t)in.n);
            if (v == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(rt);
            }
        }
        embi_js_realm_free(r);
        embi_js_runtime_free(rt);
    }
    FUZZ_GUARD_END();
    return 0;
}
