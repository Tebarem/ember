// fuzz-modules: loader js base
/* The structured-clone deserializer on raw bytes. A stream that deserializes is serialized again and
 * the result must deserialize too (every stream the serializer writes can be read back). Input:
 * byte 0 = mode (bit 0 = use the variant that also returns the transferred platform objects, bit 1 =
 * collect garbage on every allocation), rest = the serialized bytes. */
#include "fuzz_common.h"

#include "js/js.h"
#include "loader/clone.h"

/* Seed generator: with EMBER_FUZZ_DUMP_SEEDS=<dir> in the environment the process writes one valid
 * serialized stream per expression below into <dir> (with the mode byte 0 in front) and exits. The
 * committed corpus tests/fuzz/corpus/clone was made this way (see tests/fuzz/README.md). */
static const char *const seed_exprs[] = {
    "1", "-0", "1.5e300", "NaN", "'str'", "String.fromCharCode(0xD800, 0xE9) + String.fromCodePoint(0x1F600)", "true", "null", "undefined", "10n ** 40n",
    "[1, 'a', [2, [3]]]", "[1,,3]", "Object.assign([1,2], {x: 1})", "({a: 1, b: {c: [1, 2]}, 'x y': null})",
    "new Date(1e12)", "/a+(b)?/gimsuy", "new Map([[1, {a: 1}], ['k', [1]]])", "new Set([1, 'a', {}])",
    "new Error('boom', {cause: 1})", "new TypeError('t')", "new RangeError('r')",
    "new Uint8Array([1, 2, 3])", "new Float64Array([1.5, NaN])", "new BigInt64Array(2)", "new DataView(new ArrayBuffer(8), 2, 4)",
    "(() => { const a = new ArrayBuffer(16, {maxByteLength: 32}); return a; })()",
    "(() => { const o = {}; o.self = o; return o; })()", "(() => { const a = [1]; a.push(a); return a; })()",
    "new Boolean(true)", "new Number(5)", "new String('abc')", "Object(1n)",
    "(() => { const b = new ArrayBuffer(8); return {a: new Uint8Array(b), b: new Uint16Array(b, 2, 2)}; })()",
    "[new Date(0), new Date(NaN)]", "({get x() { return 1; }})", "Object.create({inherited: 1})",
};

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    const char *dir = getenv("EMBER_FUZZ_DUMP_SEEDS");
    if (dir == nullptr) {
        return 0;
    }
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = embi_js_realm_new(rt);
    for (size_t i = 0; i < sizeof seed_exprs / sizeof seed_exprs[0]; i++) {
        embi_js_value v = embi_js_eval(r, seed_exprs[i], strlen(seed_exprs[i]), "seed.js", 0);
        if (v == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
            continue;
        }
        embi_sb sb = {0};
        if (embi_clone_serialize(r, v, nullptr, 0, &sb)) {
            char path[512];
            snprintf(path, sizeof path, "%s/seed%02zu", dir, i);
            FILE *f = fopen(path, "wb");
            if (f) {
                fputc(0, f);
                fwrite(sb.data, 1, sb.len, f);
                fclose(f);
            }
        } else {
            embi_js_exception_clear(rt);
        }
        embi_sb_free(&sb);
    }
    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    exit(0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = embi_js_realm_new(rt);
    if (mode & 2) {
        embi_js_set_gc_params(rt, 0, 0.0);
    }
    embi_js_value v = EMBI_JS_UNDEFINED;
    embi_js_value transferred = EMBI_JS_UNDEFINED;
    bool ok = (mode & 1) ? embi_clone_deserialize_ex(r, fuzz_str(&in), &v, &transferred)
                         : embi_clone_deserialize(r, fuzz_str(&in), &v);
    if (!ok) {
        embi_js_exception_clear(rt);
    } else {
        embi_sb again = {0};
        if (embi_clone_serialize(r, v, nullptr, 0, &again)) {
            embi_js_value w = EMBI_JS_UNDEFINED;
            if (!embi_clone_deserialize(r, (embi_str){again.data ? again.data : "", again.len}, &w)) {
                fprintf(stderr, "a stream the serializer wrote does not deserialize\n");
                abort();
            }
        } else {
            embi_js_exception_clear(rt);
        }
        embi_sb_free(&again);
    }
    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    FUZZ_GUARD_END();
    return 0;
}
