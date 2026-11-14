/*
 * WPT url/ data: urltestdata.json (parser and getters) and setters_tests.json (API setters),
 * run as C table tests. The data is vendored under tests/wpt/url/resources.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

#include "json_lite.h"

#define URL_DATA EMBER_SOURCE_DIR "/tests/wpt/url/resources/urltestdata.json"
#define SETTERS_DATA EMBER_SOURCE_DIR "/tests/wpt/url/resources/setters_tests.json"

/* The bases WPT's relativeTo names stand for. */
static const char *relative_base(const char *name)
{
    if (strcmp(name, "non-opaque-path-base") == 0) {
        return "http://example.org/foo/bar";
    }
    if (strcmp(name, "opaque-path-base") == 0) {
        return "sc:x";
    }
    return nullptr;
}

static bool bytes_equal(const embi_sb *got, const jnode *want)
{
    return got->len == want->slen && (got->len == 0 || memcmp(got->data, want->str, got->len) == 0);
}

/* Compares one getter against the expected JSON string. */
static bool attr_matches(const embi_url *u, embi_url_attr attr, const jnode *want)
{
    embi_sb got = {nullptr, 0, 0};
    embi_url_get(u, attr, &got);
    bool ok = bytes_equal(&got, want);
    embi_sb_free(&got);
    return ok;
}

static bool origin_matches(const embi_url *u, const jnode *want)
{
    embi_sb got = {nullptr, 0, 0};
    embi_url_origin(u, &got);
    bool ok = bytes_equal(&got, want);
    embi_sb_free(&got);
    return ok;
}

/* Rows this module does not run. None are excluded: the empty list is kept so that an exclusion
 * added later has an obvious place to go, with its reason. */
static bool known_gap(const char *input)
{
    (void)input;
    return false;
}

static void report(const char *what, const char *input, const char *detail)
{
    fprintf(stderr, "  url %s: input \"%s\"%s%s\n", what, input, detail ? ": " : "", detail ? detail : "");
}

EMB_TEST(url_wpt_urltestdata)
{
    jnode *root = json_load_file(URL_DATA);
    EMB_REQUIRE(root != nullptr && root->kind == JK_ARR);
    size_t cases = 0;
    size_t failed = 0;
    for (size_t i = 0; i < root->count; i++) {
        const jnode *item = root->vals[i];
        if (item->kind != JK_OBJ) {
            continue; /* comments */
        }
        const jnode *input = json_get(item, "input");
        EMB_REQUIRE(json_is_str(input));
        const char *in = input->str;
        if (known_gap(in)) {
            continue;
        }
        cases++;

        /* The base: an explicit string, or a named base for relativeTo. */
        embi_url base = {0};
        bool have_base = false;
        const jnode *bn = json_get(item, "base");
        const jnode *rel = json_get(item, "relativeTo");
        bool base_ok = true;
        if (json_is_str(bn)) {
            base_ok = embi_url_parse((embi_str){bn->str, (uint32_t)bn->slen}, nullptr, &base);
            have_base = true;
        } else if (bn == nullptr && json_is_str(rel)) {
            /* relativeTo applies only when the row has no "base" key; "base": null means no base. */
            const char *b = relative_base(rel->str);
            base_ok = b != nullptr && embi_url_parse((embi_str){b, (uint32_t)strlen(b)}, nullptr, &base);
            have_base = true;
        }
        if (!base_ok) {
            /* The base itself does not parse: the case has no meaning here. */
            embi_url_free(&base);
            continue;
        }

        embi_url u = {0};
        bool ok = embi_url_parse((embi_str){in, (uint32_t)input->slen}, have_base ? &base : nullptr, &u);
        const jnode *failure = json_get(item, "failure");
        bool want_failure = failure != nullptr && failure->kind == JK_BOOL && failure->boolean;
        bool case_ok = true;
        if (want_failure) {
            if (ok) {
                case_ok = false;
                report("expected failure", in, nullptr);
            }
        } else if (!ok) {
            case_ok = false;
            report("failed to parse", in, nullptr);
        } else {
            static const struct {
                const char *key;
                embi_url_attr attr;
            } getters[] = {
                {"href", EMBI_URL_HREF},         {"protocol", EMBI_URL_PROTOCOL},
                {"username", EMBI_URL_USERNAME}, {"password", EMBI_URL_PASSWORD},
                {"host", EMBI_URL_HOST},         {"hostname", EMBI_URL_HOSTNAME},
                {"port", EMBI_URL_PORT},         {"pathname", EMBI_URL_PATHNAME},
                {"search", EMBI_URL_SEARCH},     {"hash", EMBI_URL_HASH},
            };
            for (size_t g = 0; g < sizeof getters / sizeof getters[0]; g++) {
                const jnode *want = json_get(item, getters[g].key);
                if (json_is_str(want) && !attr_matches(&u, getters[g].attr, want)) {
                    case_ok = false;
                    embi_sb got = {nullptr, 0, 0};
                    embi_url_get(&u, getters[g].attr, &got);
                    char detail[512];
                    snprintf(detail, sizeof detail, "%s got \"%.*s\" want \"%s\"", getters[g].key,
                             (int)got.len, got.data ? got.data : "", want->str);
                    embi_sb_free(&got);
                    report("mismatch", in, detail);
                    break;
                }
            }
            const jnode *origin = json_get(item, "origin");
            if (case_ok && json_is_str(origin) && !origin_matches(&u, origin)) {
                case_ok = false;
                report("mismatch", in, "origin");
            }
        }
        if (!case_ok) {
            failed++;
        }
        embi_url_free(&u);
        embi_url_free(&base);
    }
    fprintf(stderr, "urltestdata: %zu cases, %zu failed\n", cases, failed);
    json_free(root);
    EMB_CHECK(cases > 800);
    EMB_CHECK_EQ_INT((int)failed, 0);
}

EMB_TEST(url_wpt_setters)
{
    jnode *root = json_load_file(SETTERS_DATA);
    EMB_REQUIRE(root != nullptr && root->kind == JK_OBJ);
    static const struct {
        const char *key;
        embi_url_attr attr;
    } setters[] = {
        {"href", EMBI_URL_HREF},         {"protocol", EMBI_URL_PROTOCOL},
        {"username", EMBI_URL_USERNAME}, {"password", EMBI_URL_PASSWORD},
        {"host", EMBI_URL_HOST},         {"hostname", EMBI_URL_HOSTNAME},
        {"port", EMBI_URL_PORT},         {"pathname", EMBI_URL_PATHNAME},
        {"search", EMBI_URL_SEARCH},     {"hash", EMBI_URL_HASH},
    };
    static const struct {
        const char *key;
        embi_url_attr attr;
    } getters[] = {
        {"href", EMBI_URL_HREF},         {"protocol", EMBI_URL_PROTOCOL},
        {"username", EMBI_URL_USERNAME}, {"password", EMBI_URL_PASSWORD},
        {"host", EMBI_URL_HOST},         {"hostname", EMBI_URL_HOSTNAME},
        {"port", EMBI_URL_PORT},         {"pathname", EMBI_URL_PATHNAME},
        {"search", EMBI_URL_SEARCH},     {"hash", EMBI_URL_HASH},
    };
    size_t cases = 0;
    size_t failed = 0;
    for (size_t s = 0; s < sizeof setters / sizeof setters[0]; s++) {
        const jnode *list = json_get(root, setters[s].key);
        if (list == nullptr || list->kind != JK_ARR) {
            continue;
        }
        for (size_t i = 0; i < list->count; i++) {
            const jnode *item = list->vals[i];
            const jnode *href = json_get(item, "href");
            const jnode *value = json_get(item, "new_value");
            const jnode *expected = json_get(item, "expected");
            if (!json_is_str(href) || !json_is_str(value) || expected == nullptr) {
                continue;
            }
            cases++;
            embi_url u = {0};
            bool parsed = embi_url_parse((embi_str){href->str, (uint32_t)href->slen}, nullptr, &u);
            EMB_CHECK(parsed);
            if (!parsed) {
                failed++;
                continue;
            }
            embi_url_set(&u, setters[s].attr, (embi_str){value->str, (uint32_t)value->slen});
            bool case_ok = true;
            for (size_t g = 0; g < sizeof getters / sizeof getters[0]; g++) {
                const jnode *want = json_get(expected, getters[g].key);
                if (json_is_str(want) && !attr_matches(&u, getters[g].attr, want)) {
                    case_ok = false;
                    embi_sb got = {nullptr, 0, 0};
                    embi_url_get(&u, getters[g].attr, &got);
                    char detail[512];
                    snprintf(detail, sizeof detail, "%s.%s new \"%s\" got \"%.*s\" want \"%s\"",
                             setters[s].key, getters[g].key, value->str, (int)got.len,
                             got.data ? got.data : "", want->str);
                    embi_sb_free(&got);
                    report("setter", href->str, detail);
                    break;
                }
            }
            if (!case_ok) {
                failed++;
            }
            embi_url_free(&u);
        }
    }
    fprintf(stderr, "setters: %zu cases, %zu failed\n", cases, failed);
    json_free(root);
    EMB_CHECK(cases > 250);
    EMB_CHECK_EQ_INT((int)failed, 0);
}

LOADER_TEST_MAIN()
