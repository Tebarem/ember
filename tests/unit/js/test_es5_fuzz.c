#include "emb_test.h"

#include "base/base.h"
#include "js/compiler.h"
#include "js/js.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* Prefixes of the test262 harness files are compiled. A prefix either compiles or fails with a
 * SyntaxError, and the parser never crashes or hangs. The full text of each file is also checked:
 * it must compile, or fail only with a "not yet supported" SyntaxError (a misparse of valid ES5
 * would report some other message).
 *
 * To keep the run short, each file contributes a stride of prefixes plus a fixed-seed sample of
 * other lengths (the seed is constant, so every run tests the same prefixes). */

#define FUZZ_PREFIX_TARGET 256u /* prefixes per file by stride */
#define FUZZ_RANDOM_PER_FILE 96u /* seeded extra prefix lengths per file */

typedef struct fuzz_ctx {
    embi_js_runtime *rt;
    embi_js_realm *r;
    uint64_t rng;
    int files;
    int prefixes;
    int syntax_errors;
    int failures;
    int misparses;
} fuzz_ctx;

static uint64_t fuzz_next(fuzz_ctx *c)
{
    /* xorshift64*, seeded per run with a constant. */
    uint64_t x = c->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    c->rng = x;
    return x * 2685821657736338717ull;
}

static bool read_file(const char *path, uint8_t **out, size_t *len)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        return false;
    }
    Sint64 n = SDL_GetIOSize(io);
    if (n <= 0) {
        SDL_CloseIO(io);
        return false;
    }
    uint8_t *buf = embi_alloc((size_t)n);
    size_t got = SDL_ReadIO(io, buf, (size_t)n);
    SDL_CloseIO(io);
    *out = buf;
    *len = got;
    return got == (size_t)n;
}

/* The name and message of the pending exception: true when it is a SyntaxError whose message
 * contains want (or always, when want is nullptr). */
static bool pending_is_syntax_error(embi_js_runtime *rt, const char *want)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return false;
    }
    embi_js_value n = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "name"));
    if (!embi_js_is_string(n)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(n), &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), "SyntaxError") == 0;
    embi_sb_free(&sb);
    if (!ok || !want) {
        return ok;
    }
    embi_js_value m = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "message"));
    if (!embi_js_is_string(m)) {
        return false;
    }
    embi_sb mb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(m), &mb);
    ok = strstr(embi_sb_cstr(&mb), want) != nullptr;
    embi_sb_free(&mb);
    return ok;
}

static void compile_prefix(fuzz_ctx *c, const uint8_t *buf, size_t n, bool full)
{
    embi_js_value v = embi_js_compile(c->r, buf, (uint32_t)n, "harness.js", false);
    c->prefixes++;
    if (v == EMBI_JS_EXCEPTION) {
        if (pending_is_syntax_error(c->rt, nullptr)) {
            c->syntax_errors++;
            if (full && !pending_is_syntax_error(c->rt, "not yet supported")) {
                c->misparses++;
            }
        } else {
            c->failures++;
        }
        embi_js_exception_clear(c->rt);
    }
}

static void fuzz_one(fuzz_ctx *c, const char *path)
{
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!read_file(path, &buf, &len)) {
        return;
    }
    c->files++;
    size_t stride = len / FUZZ_PREFIX_TARGET + 1u;
    for (size_t n = 0; n < len; n += stride) {
        compile_prefix(c, buf, n, false);
    }
    for (uint32_t i = 0; i < FUZZ_RANDOM_PER_FILE && len > 0; i++) {
        compile_prefix(c, buf, (size_t)(fuzz_next(c) % len), false);
    }
    compile_prefix(c, buf, len, true);
    embi_free(buf, len);
}

static SDL_EnumerationResult visit(void *user, const char *dirname, const char *fname)
{
    fuzz_ctx *c = user;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dirname, fname);
    size_t flen = strlen(fname);
    if (flen > 3 && strcmp(fname + flen - 3, ".js") == 0) {
        fuzz_one(c, path);
    }
    return SDL_ENUM_CONTINUE;
}

EMB_TEST(es5_parser_prefix_fuzz_harness)
{
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/third_party/test262/harness", EMBER_SOURCE_DIR);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        SDL_Log("prefix fuzz skipped: %s is not present (the test262 submodule is not checked out)", dir);
        EMB_SKIP("test262 harness directory is absent");
    }
    fuzz_ctx c;
    memset(&c, 0, sizeof c);
    c.rng = 0x9E3779B97F4A7C15ull; /* fixed seed */
    c.rt = embi_js_runtime_new();
    c.r = c.rt ? embi_js_realm_new(c.rt) : nullptr;
    EMB_REQUIRE(c.r != nullptr);
    EMB_REQUIRE(SDL_EnumerateDirectory(dir, visit, &c));
    SDL_Log("prefix fuzz: %d files, %d prefixes, %d syntax errors, %d other failures, %d misparses",
            c.files, c.prefixes, c.syntax_errors, c.failures, c.misparses);
    EMB_CHECK(c.files > 0);
    EMB_CHECK(c.failures == 0);
    EMB_CHECK(c.misparses == 0);
    embi_js_realm_free(c.r);
    embi_js_runtime_free(c.rt);
}

EMB_TEST_MAIN()
