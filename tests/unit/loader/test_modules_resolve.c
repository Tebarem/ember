/*
 * The module host's specifier resolution through import maps (W7d): imports and their
 * prefix keys, scopes by the longest matching prefix of the referrer, relative and absolute specifiers, bare specifiers
 * with no entry (an error), null entries (a value that is not a URL), the merge of several maps (the first entry of a
 * key wins), the members that are skipped ("integrity"), JSON escapes in keys, and syntax errors (the map is ignored as
 * a whole). These need no JS runtime: the host is made with a null runtime, which the resolver does not use.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/modules.h"

#include <SDL3/SDL_log.h>

#include <string.h>

static const embi_mod_host k_host = {nullptr, nullptr, nullptr, nullptr, nullptr};

static bool add_map(embi_modules *m, const char *text)
{
    embi_sb err = {nullptr, 0, 0};
    bool ok = embi_modules_add_importmap(m, (embi_str){text, (uint32_t)strlen(text)}, &err);
    embi_sb_free(&err);
    return ok;
}

/* Resolves spec against base; true with the result in out (a fresh buffer, freed by the caller) on success. */
static bool resolve(embi_modules *m, const char *spec, const char *base, embi_sb *out, embi_sb *err)
{
    return embi_modules_resolve(m, (embi_str){spec, (uint32_t)strlen(spec)}, (embi_str){base, (uint32_t)strlen(base)},
                                out, err);
}

static bool resolves_to(embi_modules *m, const char *spec, const char *base, const char *want)
{
    embi_sb out = {nullptr, 0, 0};
    embi_sb err = {nullptr, 0, 0};
    bool ok = resolve(m, spec, base, &out, &err);
    bool same = ok && out.len == strlen(want) && memcmp(out.data, want, out.len) == 0;
    if (!same) {
        SDL_Log("resolve(%s, %s) gave %s (ok=%d), want %s", spec, base, ok ? embi_sb_cstr(&out) : "(none)", ok, want);
    }
    embi_sb_free(&out);
    embi_sb_free(&err);
    return same;
}

static bool fails(embi_modules *m, const char *spec, const char *base)
{
    embi_sb out = {nullptr, 0, 0};
    embi_sb err = {nullptr, 0, 0};
    bool ok = resolve(m, spec, base, &out, &err);
    embi_sb_free(&out);
    embi_sb_free(&err);
    return !ok;
}

EMB_TEST(modules_import_map_imports_and_prefixes)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    EMB_CHECK(add_map(m, "{\"imports\": {\"lodash\": \"./vendor/lodash.js\", \"pre/\": \"./pre/\"}}"));
    EMB_CHECK(resolves_to(m, "lodash", "app://local/index.html", "app://local/vendor/lodash.js"));
    EMB_CHECK(resolves_to(m, "pre/a.js", "app://local/index.html", "app://local/pre/a.js"));
    EMB_CHECK(resolves_to(m, "pre/deep/b.js", "app://local/x/y.js", "app://local/pre/deep/b.js"));
    /* A relative specifier that no entry maps resolves against the referrer, as a URL. */
    EMB_CHECK(resolves_to(m, "./b.js", "app://local/x/index.js", "app://local/x/b.js"));
    EMB_CHECK(resolves_to(m, "../c.js", "app://local/x/index.js", "app://local/c.js"));
    /* An absolute URL stays as it is. */
    EMB_CHECK(resolves_to(m, "https://cdn.test/y.js", "app://local/index.html", "https://cdn.test/y.js"));
    /* A bare specifier with no entry is an error. */
    EMB_CHECK(fails(m, "react", "app://local/index.html"));
    embi_modules_free(m);
}

EMB_TEST(modules_import_map_scopes_by_referrer)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    EMB_CHECK(add_map(m, "{\"scopes\": {\"./sub/\": {\"x\": \"./other.js\"}}}"));
    /* The scope applies to modules under sub/ only. */
    EMB_CHECK(resolves_to(m, "x", "app://local/sub/m.js", "app://local/other.js"));
    EMB_CHECK(resolves_to(m, "x", "app://local/sub/deeper/m.js", "app://local/other.js"));
    EMB_CHECK(fails(m, "x", "app://local/index.html"));
    embi_modules_free(m);
}

EMB_TEST(modules_import_map_longest_scope_wins)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    EMB_CHECK(add_map(m, "{\"scopes\": {\"./\": {\"x\": \"./outer.js\"}, \"./sub/\": {\"x\": \"./inner.js\"}}}"));
    EMB_CHECK(resolves_to(m, "x", "app://local/sub/m.js", "app://local/inner.js"));
    EMB_CHECK(resolves_to(m, "x", "app://local/other/m.js", "app://local/outer.js"));
    embi_modules_free(m);
}

EMB_TEST(modules_import_map_falls_through_to_outer_scope)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    EMB_CHECK(add_map(m, "{\"imports\": {\"t\": \"./top.js\"}, \"scopes\": {\"./\": {\"x\": \"./outer.js\"}, "
                         "\"./sub/\": {\"y\": \"./y.js\"}}}"));
    EMB_CHECK(resolves_to(m, "x", "app://local/sub/m.js", "app://local/outer.js"));
    EMB_CHECK(resolves_to(m, "y", "app://local/sub/m.js", "app://local/y.js"));
    EMB_CHECK(resolves_to(m, "t", "app://local/sub/m.js", "app://local/top.js"));
    EMB_CHECK(fails(m, "y", "app://local/other/m.js"));
    embi_modules_free(m);
}

EMB_TEST(modules_import_map_null_entries_and_skipped_members)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    /* "bad" maps to a bare specifier (not a URL): it is null, so it fails. "integrity" is skipped; a non-string value
     * is skipped too (the entry is missing). */
    EMB_CHECK(add_map(m, "{\"integrity\": {\"a.js\": \"sha384-x\"}, \"imports\": {\"bad\": \"bare\", \"n\": 5, "
                         "\"ok\": \"./ok.js\"}}"));
    EMB_CHECK(fails(m, "bad", "app://local/index.html"));
    EMB_CHECK(fails(m, "n", "app://local/index.html"));
    EMB_CHECK(resolves_to(m, "ok", "app://local/index.html", "app://local/ok.js"));
    embi_modules_free(m);
}

EMB_TEST(modules_import_map_first_entry_of_a_key_wins)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    EMB_CHECK(add_map(m, "{\"imports\": {\"k\": \"./1.js\"}}"));
    EMB_CHECK(add_map(m, "{\"imports\": {\"k\": \"./2.js\", \"j\": \"./3.js\"}}"));
    EMB_CHECK(resolves_to(m, "k", "app://local/index.html", "app://local/1.js"));
    EMB_CHECK(resolves_to(m, "j", "app://local/index.html", "app://local/3.js"));
    embi_modules_free(m);
}

EMB_TEST(modules_import_map_json_escapes_and_syntax_errors)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    /* \u0061 is "a": the key is "ab". */
    EMB_CHECK(add_map(m, "{\"imports\": {\"\\u0061b\": \"./esc.js\"}}"));
    EMB_CHECK(resolves_to(m, "ab", "app://local/index.html", "app://local/esc.js"));
    /* A syntax error ignores the whole map: nothing of it is merged. */
    EMB_CHECK(!add_map(m, "{\"imports\": {\"broken\": \"./b.js\""));
    EMB_CHECK(!add_map(m, "[]"));
    EMB_CHECK(!add_map(m, "{\"imports\": {\"half\": \"./h.js\"}} trailing"));
    EMB_CHECK(fails(m, "broken", "app://local/index.html"));
    EMB_CHECK(fails(m, "half", "app://local/index.html"));
    embi_modules_free(m);
}

EMB_TEST(modules_resolve_without_maps)
{
    embi_modules *m = embi_modules_new(nullptr, nullptr, &k_host);
    embi_modules_set_doc_url(m, EMBI_STR("app://local/index.html"));
    EMB_CHECK(resolves_to(m, "./a/../b.js", "app://local/x/", "app://local/x/b.js"));
    EMB_CHECK(resolves_to(m, "/root.js", "app://local/x/", "app://local/root.js"));
    EMB_CHECK(fails(m, "./a.js", ""));
    embi_modules_free(m);
}

LOADER_TEST_MAIN()
