#pragma once
#ifndef EMBER_CANVAS
#error "EMBER_CANVAS must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
#ifndef EMBER_SVG
#error "EMBER_SVG must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * Header-only unit test harness.
 *
 * Usage (one translation unit per test executable, the one that uses EMB_TEST_MAIN):
 *
 *     #include "emb_test.h"
 *
 *     EMB_TEST(addition_works) {
 *         EMB_CHECK_EQ_INT(2 + 2, 4);
 *     }
 *
 *     EMB_TEST_MAIN()
 *
 * Output: one line per test (PASS, FAIL or SKIP), then a summary line.
 * Exit status: 0 when no test failed and not every test skipped, 1 on any failure or when no
 * test is registered, 77 when every test skipped.
 * Registration uses GCC/Clang constructor functions. At most EMB_TEST_MAX tests per executable.
 */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

/* The MSVC C runtime (clang's Windows target) shows a modal dialog on abort() and on CRT
 * assertion failures, which blocks unattended test runs. Tests report to stderr instead. */
#if defined(_WIN32) && defined(_MSC_VER)
#include <crtdbg.h>
#include <stdlib.h>
static inline void emb_test_no_dialogs(void)
{
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
}
#else
static inline void emb_test_no_dialogs(void) {}
#endif

#define EMB_TEST_MAX 512
#define EMB_TEST_SKIP_CODE 77

/* 1 when built with AddressSanitizer. Its larger, instrumented frames leave stale pointers that
 * a conservative stack scan treats as roots, so tests that assert an unreachable cell was
 * actually freed are best-effort there (keeping live cells alive is still checked everywhere). */
#if defined(__SANITIZE_ADDRESS__)
#define EMB_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EMB_TEST_ASAN 1
#endif
#endif
#ifndef EMB_TEST_ASAN
#define EMB_TEST_ASAN 0
#endif

/* Keeps a helper out of line and out of interprocedural optimization (stack-scrubbing helpers
 * in the conservative-GC tests). GCC has noipa; Clang has no equivalent, so use optnone. */
#if defined(__clang__)
#define EMB_TEST_NOIPA __attribute__((noinline, optnone))
#else
#define EMB_TEST_NOIPA __attribute__((noinline, noipa))
#endif

typedef void (*emb_test_fn)(void);

typedef struct emb_test_entry {
    const char *name;
    emb_test_fn fn;
} emb_test_entry;

static emb_test_entry emb_test_table[EMB_TEST_MAX];
static int emb_test_count;
static int emb_test_overflow;
static int emb_test_cur_failures;
static const char *emb_test_cur_skip;

static inline void emb_test_register(const char *name, emb_test_fn fn)
{
    if (emb_test_count >= EMB_TEST_MAX) {
        emb_test_overflow = 1;
        return;
    }
    emb_test_table[emb_test_count].name = name;
    emb_test_table[emb_test_count].fn = fn;
    emb_test_count++;
}

static inline void emb_test_report_fail(const char *file, int line, const char *msg)
{
    emb_test_cur_failures++;
    SDL_Log("    %s:%d: %s", file, line, msg);
}

static inline void emb_test_report_eq_int(const char *file, int line, const char *ea,
                                          const char *eb, long long a, long long b)
{
    emb_test_cur_failures++;
    SDL_Log("    %s:%d: %s == %s failed (%lld != %lld)", file, line, ea, eb, a, b);
}

static inline void emb_test_report_eq_str(const char *file, int line, const char *ea,
                                          const char *eb, const char *a, const char *b)
{
    emb_test_cur_failures++;
    SDL_Log("    %s:%d: %s == %s failed (\"%s\" != \"%s\")", file, line, ea, eb,
            a ? a : "(null)", b ? b : "(null)");
}

static inline bool emb_test_str_equal(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return SDL_strcmp(a, b) == 0;
}

/* SZ3b: a build without EMBER_SVG or EMBER_CANVAS cannot pass the pages of the suites that use them (WPT, reftests). The runners of
 * those suites call this with the path of a FAILING test's main file: true when the page uses a feature that is off, so the
 * failure is not counted as unexpected. It looks for specific markers (an <svg> or <canvas> element, getContext,
 * createImageBitmap/ImageBitmap, url(#id), a .svg file, the image/svg media type, the SVG namespace URI, an SVG interface
 * name such as SVGMarkerElement), not for the words, which also appear in the
 * Canvas and CanvasText system colors, in prose and in file names. A test that passes is never excused: an unexpected pass is
 * always reported. Always false when both features are on. */
static inline bool emb_test_has_svg_interface_name(const char *text)
{
    for (const char *p = SDL_strstr(text, "SVG"); p != nullptr; p = SDL_strstr(p + 3, "SVG")) {
        if (p[3] >= 'A' && p[3] <= 'Z') {
            return true;
        }
    }
    return false;
}

static inline bool emb_test_source_needs_disabled_feature(const char *path)
{
#if EMBER_SVG && EMBER_CANVAS
    (void)path;
    return false;
#else
    size_t n = 0;
    char *text = (char *)SDL_LoadFile(path, &n);
    if (text == nullptr) {
        return false;
    }
    bool r = false;
#if !EMBER_SVG
    r = r || SDL_strcasestr(text, "<svg") != nullptr || SDL_strcasestr(text, ".svg") != nullptr ||
        SDL_strcasestr(text, "image/svg") != nullptr || SDL_strstr(text, "url(#") != nullptr ||
        SDL_strstr(text, "www.w3.org/2000/svg") != nullptr || emb_test_has_svg_interface_name(text);
#endif
#if !EMBER_CANVAS
    r = r || SDL_strcasestr(text, "<canvas") != nullptr || SDL_strstr(text, "getContext") != nullptr ||
        SDL_strstr(text, "createImageBitmap") != nullptr || SDL_strstr(text, "ImageBitmap") != nullptr;
#endif
    SDL_free(text);
    return r;
#endif
}

static inline int emb_test_run_all(void)
{
    emb_test_no_dialogs();
    /* Runtimes made by tests see no system fonts (EMBER_NO_SYSTEM_FONTS, read by emb_runtime_create), so results do not
     * depend on the machine's fonts. A test of system fonts unsets it. An environment that sets it already wins. */
    SDL_setenv_unsafe("EMBER_NO_SYSTEM_FONTS", "1", 0);
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
    if (emb_test_overflow) {
        SDL_Log("too many tests (max %d)", EMB_TEST_MAX);
        return 1;
    }
    if (emb_test_count == 0) {
        SDL_Log("no tests registered");
        return 1;
    }

    for (int i = 0; i < emb_test_count; i++) {
        emb_test_cur_failures = 0;
        emb_test_cur_skip = nullptr;
        emb_test_table[i].fn();

        if (emb_test_cur_failures > 0) {
            failed++;
            SDL_Log("FAIL %s", emb_test_table[i].name);
        } else if (emb_test_cur_skip != nullptr) {
            skipped++;
            SDL_Log("SKIP %s (%s)", emb_test_table[i].name, emb_test_cur_skip);
        } else {
            passed++;
            SDL_Log("PASS %s", emb_test_table[i].name);
        }
    }

    SDL_Log("%d passed, %d failed, %d skipped", passed, failed, skipped);
    if (failed > 0) {
        return 1;
    }
    if (skipped == emb_test_count) {
        return EMB_TEST_SKIP_CODE;
    }
    return 0;
}

#define EMB_TEST(name)                                                                         \
    static void emb_test_fn_##name(void);                                                      \
    __attribute__((constructor, used)) static void emb_test_reg_##name(void)                   \
    {                                                                                          \
        emb_test_register(#name, emb_test_fn_##name);                                          \
    }                                                                                          \
    static void emb_test_fn_##name(void)

#define EMB_CHECK(cond)                                                                        \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            emb_test_report_fail(__FILE__, __LINE__, "check failed: " #cond);                  \
        }                                                                                      \
    } while (0)

#define EMB_REQUIRE(cond)                                                                      \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            emb_test_report_fail(__FILE__, __LINE__, "require failed: " #cond);                \
            return;                                                                            \
        }                                                                                      \
    } while (0)

#define EMB_CHECK_EQ_INT(a, b)                                                                 \
    do {                                                                                       \
        long long emb_test_a_ = (long long)(a);                                                \
        long long emb_test_b_ = (long long)(b);                                                \
        if (emb_test_a_ != emb_test_b_) {                                                      \
            emb_test_report_eq_int(__FILE__, __LINE__, #a, #b, emb_test_a_, emb_test_b_);      \
        }                                                                                      \
    } while (0)

#define EMB_CHECK_EQ_STR(a, b)                                                                 \
    do {                                                                                       \
        const char *emb_test_sa_ = (a);                                                        \
        const char *emb_test_sb_ = (b);                                                        \
        if (!emb_test_str_equal(emb_test_sa_, emb_test_sb_)) {                                 \
            emb_test_report_eq_str(__FILE__, __LINE__, #a, #b, emb_test_sa_, emb_test_sb_);    \
        }                                                                                      \
    } while (0)

#define EMB_SKIP(reason)                                                                       \
    do {                                                                                       \
        emb_test_cur_skip = (reason);                                                          \
        return;                                                                                \
    } while (0)

#define EMB_TEST_MAIN()                                                                        \
    int main(int argc, char **argv)                                                            \
    {                                                                                          \
        (void)argc;                                                                            \
        (void)argv;                                                                            \
        return emb_test_run_all();                                                             \
    }
