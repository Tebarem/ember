// fuzz-modules: js jslex num base
/* JSON.parse (the intrinsic parser) and number parsing/formatting. Input: byte 0 = mode, rest = text.
 * Modes (byte 0 & 3): 0 JSON.parse of the text; 1 decimal number parsing, cross-checked against the
 * C library's correctly rounded strtod and against the lexer's own conversion; 2 radix parsing and
 * every formatting function on a double built from the first 8 bytes (shortest output must parse
 * back to the same double); 3 JSON.parse of the text, then JSON.stringify and parse again in script. */
#include "fuzz_common.h"

#include "js/js.h"
#include "jslex/jslex.h"
#include "num/num.h"

#include <math.h>

static bool same_double(double a, double b)
{
    if (isnan(a) && isnan(b)) {
        return true;
    }
    return memcmp(&a, &b, sizeof a) == 0;
}

/* True when s[0..n) is digits [. digits] [e [+-] digits], the grammar of the lexer's converter. */
static bool lexer_grammar(const char *s, size_t n)
{
    size_t i = 0;
    size_t d = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        i++;
        d++;
    }
    if (d == 0) {
        return false;
    }
    if (i < n && s[i] == '.') {
        i++;
        size_t f = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
            f++;
        }
        if (f == 0) {
            return false;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        size_t e = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
            e++;
        }
        if (e == 0) {
            return false;
        }
    }
    return i == n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in) & 3;
    char out[EMBI_NUM_STR_MAX];
    if (mode == 0 || mode == 3) {
        embi_js_runtime *rt = embi_js_runtime_new();
        embi_js_realm *r = embi_js_realm_new(rt);
        embi_js_string *src = embi_js_str_new_utf8(rt, fuzz_str(&in).ptr, in.n);
        embi_js_value v = embi_js_json_parse_text(rt, src);
        if (v == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
        } else if (mode == 3) {
            (void)embi_js_put(rt, r->global, embi_js_key_c(rt, "v"), v, false);
            static const char js[] = "var s = JSON.stringify(v); if (s !== undefined) JSON.parse(s);";
            embi_js_value w = embi_js_eval(r, js, sizeof js - 1, "json.js", 0);
            if (w == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(rt);
            }
        }
        embi_js_realm_free(r);
        embi_js_runtime_free(rt);
    } else if (mode == 1) {
        size_t used = 0;
        double d = embi_num_parse_decimal(fuzz_str(&in).ptr, in.n, &used);
        if (used > in.n) {
            abort();
        }
        if (used > 0) {
            char *z = embi_alloc(used + 1);
            memcpy(z, in.p, used);
            z[used] = '\0';
            /* strtod accepts the same grammar for these bytes (sign, digits, '.', exponent). */
            double ref = strtod(z, nullptr);
            if (!same_double(d, ref)) {
                fprintf(stderr, "decimal mismatch: '%s' got %a want %a\n", z, d, ref);
                abort();
            }
            const char *body = z;
            size_t bn = used;
            if (body[0] == '+' || body[0] == '-') {
                body++;
                bn--;
            }
            if (lexer_grammar(body, bn)) {
                double lx = embi_jslex_decimal_to_double(body, (uint32_t)bn);
                if (!same_double(fabs(d), lx)) {
                    fprintf(stderr, "lexer mismatch: '%s' got %a want %a\n", z, lx, fabs(d));
                    abort();
                }
            }
            embi_free(z, used + 1);
        }
    } else {
        double d = 0;
        if (in.n >= 8) {
            memcpy(&d, in.p, 8);
        }
        size_t n = embi_num_to_str(d, out);
        if (isfinite(d)) {
            size_t used = 0;
            double back = embi_num_parse_decimal(out, n, &used);
            if (used != n || !(back == d)) {
                fprintf(stderr, "round trip: %a -> '%s' -> %a\n", d, out, back);
                abort();
            }
        }
        unsigned radix = 2 + (in.n > 8 ? in.p[8] : 0) % 35;
        unsigned digits = in.n > 9 ? in.p[9] % 101 : 0;
        (void)embi_num_to_radix_str(d, radix, out);
        (void)embi_num_to_fixed(d, digits, out);
        (void)embi_num_to_exponential(d, (int)digits - 1, out);
        (void)embi_num_to_precision(d, digits == 0 ? 1 : digits, out);
        size_t used = 0;
        const char *tail = in.n > 10 ? (const char *)in.p + 10 : "";
        (void)embi_num_parse_radix(tail, in.n > 10 ? in.n - 10 : 0, radix, &used);
    }
    FUZZ_GUARD_END();
    return 0;
}
