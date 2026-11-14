/*
 * The runtime of the embedding layer (W4e): create and destroy without leaks, the resource handlers and their scheme
 * rules, the file loader's directory confinement, and the font entry point's refusal of bad data. No GPU needed.
 */
#include "emb_test.h"

#include "embed/embed_int.h"
#include "embed/url.h"
#include "style/style.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

static bool payload_handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    (void)url;
    (void)url_len;
    *(int *)user += 1;
    /* The library sets the size of the struct before the call (the struct is versioned). */
    if (out->size != sizeof(emb_resource) || out->data != nullptr || out->len != 0) {
        return false;
    }
    static const uint8_t bytes[] = "payload";
    out->data = bytes;
    out->len = sizeof bytes - 1;
    return true;
}

static void quiet_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    (void)level;
    (void)message;
}

/* FP2b: the runtime parses the shared UA sheet on a thread of its own and joins it at destroy, whenever that is. */
EMB_TEST(runtime_ua_prefetch_is_joined_at_destroy_and_shared)
{
    emb_runtime_destroy(emb_runtime_create(nullptr)); /* the process-wide tables are made once and kept */
    size_t base = embi_mem_live();
    EMB_CHECK(embi_style_ua_users() == 0);
    emb_runtime *a = emb_runtime_create(nullptr);
    emb_runtime_destroy(a); /* destroyed at once: the thread may still be parsing */
    EMB_CHECK(embi_style_ua_users() == 0);
    emb_runtime *r1 = emb_runtime_create(nullptr);
    emb_runtime *r2 = emb_runtime_create(nullptr);
    EMB_REQUIRE(r1 != nullptr && r2 != nullptr);
    embi_css_sheet *s = embi_style_ua_sheet(); /* waits for the parse when a thread has the lock */
    EMB_CHECK(s != nullptr);
    EMB_CHECK(embi_style_ua_users() >= 1 && embi_style_ua_users() <= 3); /* the two threads may not have run yet */
    embi_style_ua_release(s);
    emb_runtime_destroy(r1);
    emb_runtime_destroy(r2);
    EMB_CHECK(embi_style_ua_users() == 0);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(runtime_create_destroy_returns_to_baseline)
{
    /* One cycle first: the atom table and the text system's shared tables are created once and kept. */
    emb_runtime *warm = emb_runtime_create(nullptr);
    EMB_REQUIRE(warm != nullptr);
    emb_runtime_destroy(warm);
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        emb_runtime *rt = emb_runtime_create(nullptr);
        EMB_REQUIRE(rt != nullptr);
        EMB_CHECK(rt->runtime_init_ms >= 0.0);
        emb_runtime_destroy(rt);
    }
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(runtime_handler_schemes_and_fetch)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    int calls = 0;
    /* The built-in schemes cannot be replaced, and a scheme must be a valid one. */
    EMB_CHECK(!emb_runtime_set_resource_handler(rt, "file", payload_handler, &calls));
    EMB_CHECK(!emb_runtime_set_resource_handler(rt, "data", payload_handler, &calls));
    EMB_CHECK(!emb_runtime_set_resource_handler(rt, "1bad", payload_handler, &calls));
    EMB_CHECK(!emb_runtime_set_resource_handler(rt, "", payload_handler, &calls));
    EMB_CHECK(emb_runtime_set_resource_handler(rt, "Mem", payload_handler, &calls));

    uint8_t *bytes = nullptr;
    size_t len = 0;
    EMB_CHECK(embi_rt_fetch(rt, EMBI_STR("mem://a/b"), &bytes, &len));
    EMB_CHECK(len == 7 && memcmp(bytes, "payload", 7) == 0);
    SDL_free(bytes);
    EMB_CHECK(calls == 1);
    /* No network: a scheme without a handler is refused, and the handler is not called for it. */
    EMB_CHECK(!embi_rt_fetch(rt, EMBI_STR("https://example.com/x"), &bytes, &len));
    EMB_CHECK(calls == 1);
    /* Removing the handler makes its scheme unknown again. */
    EMB_CHECK(emb_runtime_set_resource_handler(rt, "mem", nullptr, nullptr));
    EMB_CHECK(!embi_rt_fetch(rt, EMBI_STR("mem://a/b"), &bytes, &len));
    emb_runtime_destroy(rt);
}

EMB_TEST(runtime_file_loader_is_confined_to_allowed_dirs)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    /* Relative directories are refused. */
    EMB_CHECK(!emb_runtime_allow_path(rt, "relative/dir"));
    EMB_CHECK(!emb_runtime_allow_path(rt, ""));

    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/tests/unit/embed", EMBER_SOURCE_DIR);
    EMB_CHECK(emb_runtime_allow_path(rt, dir));

    char file[1024];
    uint8_t *bytes = nullptr;
    size_t len = 0;

    /* A file under the allowed directory loads, through its file: URL. */
    SDL_snprintf(file, sizeof file, "%s/tests/unit/embed/test_embed_url.c", EMBER_SOURCE_DIR);
    char url[2048];
    size_t un = embi_url_from_path((embi_str){file, (uint32_t)strlen(file)}, url, sizeof url);
    EMB_REQUIRE(un < sizeof url);
    EMB_CHECK(embi_rt_fetch(rt, (embi_str){url, (uint32_t)un}, &bytes, &len));
    EMB_CHECK(len > 0);
    SDL_free(bytes);
    bytes = nullptr;

    /* A file outside it is denied, even though it exists. */
    SDL_snprintf(file, sizeof file, "%s/assets/fonts/OFL.txt", EMBER_SOURCE_DIR);
    un = embi_url_from_path((embi_str){file, (uint32_t)strlen(file)}, url, sizeof url);
    EMB_CHECK(!embi_rt_fetch(rt, (embi_str){url, (uint32_t)un}, &bytes, &len));
    EMB_CHECK(bytes == nullptr);

    /* A ".." segment is refused before the directory check. */
    SDL_snprintf(file, sizeof file, "file://%s/tests/unit/embed/../../../assets/fonts/OFL.txt", EMBER_SOURCE_DIR);
    EMB_CHECK(!embi_rt_fetch(rt, (embi_str){file, (uint32_t)strlen(file)}, &bytes, &len));

    /* A missing file inside the directory fails cleanly. */
    SDL_snprintf(file, sizeof file, "%s/tests/unit/embed/no_such_file.html", EMBER_SOURCE_DIR);
    un = embi_url_from_path((embi_str){file, (uint32_t)strlen(file)}, url, sizeof url);
    EMB_CHECK(!embi_rt_fetch(rt, (embi_str){url, (uint32_t)un}, &bytes, &len));
    emb_runtime_destroy(rt);
}

/* The root directory allows every absolute path. A directory that is not an ancestor refuses the file. On Windows a
 * drive directory is matched without regard to case. */
EMB_TEST(runtime_allow_root_and_drive_case)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char path[1024], url[2048];
    SDL_snprintf(path, sizeof path, "%s/tests/pages/static-todomvc.html", EMBER_SOURCE_DIR);
    size_t un = embi_url_from_path((embi_str){path, (uint32_t)strlen(path)}, url, sizeof url);
    uint8_t *bytes = nullptr;
    size_t len = 0;
    EMB_CHECK(emb_runtime_allow_path(rt, "/nonexistent-ember-dir"));
    EMB_CHECK(!embi_rt_fetch(rt, (embi_str){url, (uint32_t)un}, &bytes, &len));
    EMB_CHECK(emb_runtime_allow_path(rt, "/"));
    EMB_CHECK(embi_rt_fetch(rt, (embi_str){url, (uint32_t)un}, &bytes, &len));
    EMB_CHECK(len > 0);
    SDL_free(bytes);
    emb_runtime_destroy(rt);

#if defined(_WIN32)
    /* The same file under a drive directory spelled in lower case. */
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/tests/pages", EMBER_SOURCE_DIR);
    for (char *c = dir; *c != '\0'; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c = (char)(*c - 'A' + 'a');
        }
    }
    rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_allow_path(rt, dir));
    bytes = nullptr;
    len = 0;
    EMB_CHECK(embi_rt_fetch(rt, (embi_str){url, (uint32_t)un}, &bytes, &len));
    SDL_free(bytes);
    emb_runtime_destroy(rt);
#endif
}

EMB_TEST(runtime_data_urls_load_without_handlers)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    uint8_t *bytes = nullptr;
    size_t len = 0;
    EMB_CHECK(embi_rt_fetch(rt, EMBI_STR("data:text/plain;base64,aGVsbG8="), &bytes, &len));
    EMB_CHECK(len == 5 && memcmp(bytes, "hello", 5) == 0);
    SDL_free(bytes);
    emb_runtime_destroy(rt);
}

EMB_TEST(runtime_add_font_accepts_a_face)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/assets/fonts/ember-default.ttf", EMBER_SOURCE_DIR);
    size_t n = 0;
    void *ttf = SDL_LoadFile(path, &n);
    EMB_REQUIRE(ttf != nullptr);
    EMB_CHECK(emb_runtime_add_font(rt, ttf, n));
    /* The bytes were copied: the caller's buffer can go. */
    SDL_free(ttf);
    emb_runtime_destroy(rt);
}

EMB_TEST(runtime_flag_no_default_font_is_refused)
{
    emb_runtime_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.flags = EMB_RUNTIME_NO_DEFAULT_FONT;
    d.log = quiet_log;
    EMB_CHECK(emb_runtime_create(&d) == nullptr);
}

EMB_TEST(runtime_add_font_refuses_bad_data)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    uint8_t junk[64];
    memset(junk, 0x5A, sizeof junk);
    EMB_CHECK(!emb_runtime_add_font(rt, junk, sizeof junk));
    EMB_CHECK(!emb_runtime_add_font(rt, nullptr, 0));
    emb_runtime_destroy(rt);
}

EMB_TEST(runtime_allow_path_list_is_bounded)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    uint32_t added = 0;
    for (uint32_t i = 0; i < EMBI_RT_MAX_DIRS + 4; i++) {
        char dir[64];
        SDL_snprintf(dir, sizeof dir, "/srv/allowed/%u", (unsigned)i);
        if (emb_runtime_allow_path(rt, dir)) {
            added++;
        }
    }
    EMB_CHECK(added == EMBI_RT_MAX_DIRS);
    emb_runtime_destroy(rt);
}

EMB_TEST_MAIN()
