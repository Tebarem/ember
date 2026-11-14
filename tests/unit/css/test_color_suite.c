/*
 * Runs the color_*.json files of tests/data/css-parsing-tests. Each file is an array of
 * (input, expected) pairs. An expected null means the input must be an invalid color. Otherwise
 * the parsed color must serialize exactly to the expected string. Failing cases must be listed in
 * tests/css/color_expectations.txt, one per line as "<file> <case index> <reason>". A listed case
 * that passes is an error, so the list stays exact.
 */
#include "emb_test.h"

#include "css/props.h"
#include "json_mini.h"

#include <SDL3/SDL_iostream.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define COLOR_DIR EMBER_SOURCE_DIR "/tests/data/css-parsing-tests/"
#define EXPECT_PATH EMBER_SOURCE_DIR "/tests/css/color_expectations.txt"
#define MAX_EXPECT 512

static const char *const color_files[] = {
    "color_function_4.json",  "color_functions_5.json", "color_hexadecimal_3.json",
    "color_hexadecimal_4.json", "color_hsl_3.json",     "color_hsl_4.json",
    "color_hwb_4.json",       "color_keywords_3.json",  "color_keywords_4.json",
    "color_lab_4.json",       "color_lch_4.json",       "color_oklab_4.json",
    "color_oklch_4.json",
};

typedef struct expect_entry {
    char file[64];
    uint32_t index;
} expect_entry;

static expect_entry expects[MAX_EXPECT];
static uint32_t expect_count;
static bool expects_loaded;

static void load_expectations(void)
{
    if (expects_loaded) {
        return;
    }
    expects_loaded = true;
    size_t n = 0;
    char *text = SDL_LoadFile(EXPECT_PATH, &n);
    if (!text) {
        return;
    }
    size_t pos = 0;
    while (pos < n && expect_count < MAX_EXPECT) {
        size_t end = pos;
        while (end < n && text[end] != '\n') {
            end++;
        }
        char line[256];
        size_t len = end - pos;
        if (len > 0 && len < sizeof line) {
            memcpy(line, text + pos, len);
            line[len] = '\0';
            if (line[0] != '#' && line[0] != '\0') {
                expect_entry *e = &expects[expect_count];
                char *sp = strchr(line, ' ');
                if (sp) {
                    size_t fl = (size_t)(sp - line);
                    if (fl < sizeof e->file) {
                        memcpy(e->file, line, fl);
                        e->file[fl] = '\0';
                        e->index = (uint32_t)strtoul(sp + 1, nullptr, 10);
                        expect_count++;
                    }
                }
            }
        }
        pos = end + 1;
    }
    SDL_free(text);
}

static bool is_listed(const char *file, uint32_t index)
{
    for (uint32_t i = 0; i < expect_count; i++) {
        if (expects[i].index == index && strcmp(expects[i].file, file) == 0) {
            return true;
        }
    }
    return false;
}

/* Exact string comparison, except that numbers may differ by up to 2e-6 (the reference generator
 * computes hwb grays in a slightly different float order; the printed values differ in the 7th
 * digit). Everything that is not part of a number must match byte for byte. */
static bool serialized_equal(const char *a, size_t al, const char *b, size_t bl)
{
    if (al == bl && memcmp(a, b, al) == 0) {
        return true;
    }
    size_t i = 0, j = 0;
    while (i < al && j < bl) {
        bool na = (a[i] >= '0' && a[i] <= '9') || a[i] == '.' || (a[i] == '-' && i + 1 < al && (a[i + 1] >= '0' && a[i + 1] <= '9'));
        bool nb = (b[j] >= '0' && b[j] <= '9') || b[j] == '.' || (b[j] == '-' && j + 1 < bl && (b[j + 1] >= '0' && b[j + 1] <= '9'));
        if (na && nb) {
            char ta[64], tb[64];
            size_t ka = 0, kb = 0;
            while (i < al && ka < sizeof ta - 1 &&
                   ((a[i] >= '0' && a[i] <= '9') || a[i] == '.' || a[i] == '-' || a[i] == 'e' || a[i] == '+')) {
                ta[ka++] = a[i++];
            }
            while (j < bl && kb < sizeof tb - 1 &&
                   ((b[j] >= '0' && b[j] <= '9') || b[j] == '.' || b[j] == '-' || b[j] == 'e' || b[j] == '+')) {
                tb[kb++] = b[j++];
            }
            ta[ka] = tb[kb] = 0;
            double da = strtod(ta, nullptr), db = strtod(tb, nullptr);
            if (!(fabs(da - db) <= 2e-6)) {
                return false;
            }
        } else if (a[i] == b[j]) {
            i++;
            j++;
        } else {
            return false;
        }
    }
    return i == al && j == bl;
}

/* Runs one file. Returns the number of unexpected failures and the number of listed cases that
 * passed. */
static void run_file(const char *name, uint32_t *failures, uint32_t *stale, uint32_t *total)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s%s", COLOR_DIR, name);
    size_t n = 0;
    char *text = SDL_LoadFile(path, &n);
    if (!text) {
        emb_test_report_fail(__FILE__, __LINE__, "cannot load color fixture");
        return;
    }
    jm_val root;
    if (!jm_parse(text, n, &root) || root.type != JM_ARRAY || (root.count & 1) != 0) {
        emb_test_report_fail(__FILE__, __LINE__, "bad color fixture JSON");
        SDL_free(text);
        return;
    }
    embi_arena a;
    embi_arena_init(&a, 4096);
    for (size_t k = 0; k + 1 < root.count; k += 2) {
        const jm_val *in = &root.items[k];
        const jm_val *want = &root.items[k + 1];
        uint32_t index = (uint32_t)(k / 2);
        (*total)++;
        embi_arena_reset(&a);
        embi_css_slice cvs = embi_css_parse_cvs(&a, (embi_str){in->str, (uint32_t)in->len});
        bool pass = false;
        bool ok = false;
        char got[512] = "";
        if (want->type == JM_ARRAY) {
            /* A light-dark() value: the light and dark colors, or nothing for invalid input. */
            embi_longhand lh[1];
            ok = embi_css_parse_decl(&a, EMBI_PROP_color, cvs, lh, 1) == 1 &&
                 lh[0].value.kind == EMBI_V_LIGHT_DARK;
            if (want->count != 2) {
                pass = false;
            } else if (want->items[0].type == JM_NULL && want->items[1].type == JM_NULL) {
                pass = !ok;
            } else if (!ok) {
                pass = false;
            } else {
                pass = true;
                for (uint32_t j = 0; j < 2 && pass; j++) {
                    const jm_val *w = &want->items[j];
                    embi_sb sb = {0};
                    embi_value_serialize(&sb, &lh[0].value.pair[j]);
                    if (w->type != JM_STRING || sb.len != w->len ||
                        memcmp(sb.data, w->str, w->len) != 0) {
                        pass = false;
                        size_t gl = sb.len < sizeof got - 1 ? sb.len : sizeof got - 1;
                        memcpy(got, sb.data ? sb.data : "", gl);
                        got[gl] = '\0';
                    }
                    embi_sb_free(&sb);
                }
            }
        } else {
            embi_color c;
            ok = embi_color_parse(cvs, &c);
            if (want->type == JM_NULL) {
                pass = !ok;
            } else if (!ok || want->type != JM_STRING) {
                pass = false;
            } else {
                embi_sb sb = {0};
                embi_color_serialize(&sb, &c);
                size_t gl = sb.len < sizeof got - 1 ? sb.len : sizeof got - 1;
                memcpy(got, sb.data ? sb.data : "", gl);
                got[gl] = '\0';
                pass = serialized_equal(sb.data ? sb.data : "", sb.len, want->str, want->len);
                embi_sb_free(&sb);
            }
        }
        bool listed = is_listed(name, index);
        if (!pass && !listed) {
            (*failures)++;
            if (*failures <= 12) {
                const char *exp = want->type == JM_STRING ? want->str
                                  : want->type == JM_NULL ? "null"
                                                          : "[pair]";
                fprintf(stderr, "  %s #%u: input \"%.*s\" expected %s got %s\n", name, index,
                        (int)in->len, in->str, exp, ok ? got : "(invalid)");
            }
        } else if (pass && listed) {
            (*stale)++;
            fprintf(stderr, "  %s #%u now passes but is listed\n", name, index);
        }
    }
    embi_arena_free(&a);
    jm_free(&root);
    SDL_free(text);
}

EMB_TEST(color_css_parsing_suite)
{
    load_expectations();
    uint32_t failures = 0;
    uint32_t stale = 0;
    uint32_t total = 0;
    for (size_t i = 0; i < sizeof color_files / sizeof color_files[0]; i++) {
        run_file(color_files[i], &failures, &stale, &total);
    }
    fprintf(stderr, "color suite: %u cases, %u unexpected failures, %u stale entries\n", total,
            failures, stale);
    EMB_CHECK_EQ_INT(failures, 0);
    EMB_CHECK_EQ_INT(stale, 0);
    EMB_CHECK(total > 4000);
}

EMB_TEST_MAIN()
