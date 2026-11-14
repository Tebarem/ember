// fuzz-modules: loader js base
/* Import maps and module specifier resolution (the module host in src/loader/modules.c). Input:
 * byte 0 = mode; the rest is split at NUL bytes: pieces 0 and 1 are import map texts (the second
 * one is merged into the first), the others are specifiers resolved against the document URL, a
 * referrer, or a second URL taken from the input. */
#include "fuzz_common.h"

#include "js/js.h"
#include "loader/modules.h"

static embi_str piece(fuzz_in *in)
{
    const uint8_t *nul = in->n ? memchr(in->p, 0, in->n) : nullptr;
    size_t n = nul ? (size_t)(nul - in->p) : in->n;
    static const char empty[1] = {0};
    embi_str s = {n ? (const char *)in->p : empty, (uint32_t)n};
    in->p += n;
    in->n -= n;
    if (nul) {
        in->p++;
        in->n--;
    }
    return s;
}

static bool load_none(void *user, embi_str url, uint8_t **data, size_t *len, embi_sb *mime)
{
    (void)user;
    (void)url;
    (void)data;
    (void)len;
    (void)mime;
    return false;
}

static void warn_none(void *user, const char *text)
{
    (void)user;
    (void)text;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = embi_js_realm_new(rt);
    embi_mod_host host = {nullptr, load_none, warn_none, nullptr, nullptr};
    embi_modules *m = embi_modules_new(rt, r, &host);
    embi_modules_attach(m);
    embi_modules_set_doc_url(m, (mode & 1) ? EMBI_STR("https://example.com/dir/page.html") : EMBI_STR(""));
    embi_modules_set_script_url(m, (mode & 2) ? EMBI_STR("https://example.com/s/script.js") : EMBI_STR(""));

    embi_sb err = {0};
    embi_str map1 = piece(&in);
    (void)embi_modules_add_importmap(m, map1, &err);
    embi_str map2 = piece(&in);
    (void)embi_modules_add_importmap(m, map2, &err);
    embi_sb_clear(&err);

    unsigned n = 0;
    while (in.n > 0 && n++ < 16) {
        embi_str spec = piece(&in);
        embi_str base = (n & 1) ? EMBI_STR("https://example.com/base/mod.js") : piece(&in);
        embi_sb out = {0};
        (void)embi_modules_resolve(m, spec, base, &out, &err);
        embi_sb_free(&out);
        embi_sb_clear(&err);
    }
    embi_sb_free(&err);
    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    embi_modules_free(m);
    FUZZ_GUARD_END();
    return 0;
}
