#pragma once

/*
 * RegExp engine: the ECMAScript pattern grammar (ES2025, with the Annex B web-compat grammar for
 * patterns without u or v) compiled to a compact bytecode, and a backtracking matcher with an
 * explicit backtrack stack. The engine knows nothing about JS objects: patterns are UTF-16 code
 * units, and inputs are Latin-1 bytes or UTF-16 code units, read in place.
 *
 * Typical use from the RegExp builtins (J4c):
 *   uint32_t flags;
 *   embi_re_parse_flags(fl, fl_len, &flags);
 *   embi_re_error err;
 *   embi_re_prog *prog = embi_re_compile(src, src_len, flags, &err);   // SyntaxError on nullptr
 *   uint32_t *caps = malloc(embi_re_capture_slots(prog) * sizeof *caps);
 *   embi_re_status st = embi_re_search(prog, input, last_index, caps, nullptr);
 *
 * Programs are immutable after compilation and may be shared. All matching state lives in the call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_re_prog embi_re_prog;

/* Flag bits, one per RegExp flag letter. The d flag has no engine effect (it only asks for match
 * indices, which the builtins build from the captures). */
enum {
    EMBI_RE_FLAG_D = 1u << 0,
    EMBI_RE_FLAG_G = 1u << 1,
    EMBI_RE_FLAG_I = 1u << 2,
    EMBI_RE_FLAG_M = 1u << 3,
    EMBI_RE_FLAG_S = 1u << 4,
    EMBI_RE_FLAG_U = 1u << 5,
    EMBI_RE_FLAG_V = 1u << 6,
    EMBI_RE_FLAG_Y = 1u << 7,
};

/* Capture slots that have no value. */
#define EMBI_RE_UNDEF UINT32_MAX

typedef struct embi_re_error {
    uint32_t pos;     /* index of the offending code unit in the pattern */
    const char *msg;  /* static text, for example "unterminated group" */
} embi_re_error;

/* Parses a flags string ("dgimsuvy", each at most once, and not both u and v). Returns false on
 * any error; *flags then holds nothing useful. */
bool embi_re_parse_flags(const uint16_t *s, uint32_t len, uint32_t *flags);

/* Compiles a pattern. Returns nullptr and fills *err (err may be nullptr) on a syntax error. */
embi_re_prog *embi_re_compile(const uint16_t *pattern, uint32_t len, uint32_t flags,
                              embi_re_error *err);
void embi_re_free(embi_re_prog *prog);

uint32_t embi_re_flags(const embi_re_prog *prog);
/* Number of capturing groups, not counting the whole match (group 0). */
uint32_t embi_re_group_count(const embi_re_prog *prog);
/* Number of entries in a captures array: 2 * (group_count + 1). */
uint32_t embi_re_capture_slots(const embi_re_prog *prog);
/* The UTF-16 name of capture group 'group' (1-based), or nullptr if it is not named. */
const uint16_t *embi_re_group_name(const embi_re_prog *prog, uint32_t group, uint32_t *len);

/* An input string: Latin-1 (wide == false, units are uint8_t) or UTF-16 (wide == true, units are
 * uint16_t). The engine reads it in place. */
typedef struct embi_re_input {
    const void *units;
    uint32_t len;
    bool wide;
} embi_re_input;

typedef struct embi_re_limits {
    size_t max_bytes;                  /* cap on backtracking memory; 0 means the default (32 MB) */
    bool (*interrupt)(void *ctx);      /* polled every few thousand steps; true stops the match */
    void *ctx;
} embi_re_limits;

typedef enum embi_re_status {
    EMBI_RE_MATCH,
    EMBI_RE_NO_MATCH,
    EMBI_RE_INTERRUPTED,  /* the interrupt hook asked to stop */
    EMBI_RE_TOO_COMPLEX,  /* the backtracking memory cap was reached: "regular expression too complex" */
} embi_re_status;

/* Finds the first match starting at or after 'start' (advancing by code point in u and v mode).
 * On EMBI_RE_MATCH, caps[2i] and caps[2i+1] hold the start and end of group i, or EMBI_RE_UNDEF.
 * 'caps' must hold embi_re_capture_slots(prog) entries. Precondition: start <= in.len; a larger
 * start returns EMBI_RE_NO_MATCH without reading the input (RegExpBuiltinExec checks lastIndex
 * against the length first). in.units may be nullptr when in.len is 0.
 * In u and v mode a start that falls between a lead and a trail surrogate is moved back to the
 * lead (the code point containing it), and the captures then begin there. */
embi_re_status embi_re_search(const embi_re_prog *prog, embi_re_input in, uint32_t start,
                              uint32_t *caps, const embi_re_limits *limits);

/* Matches only at 'start' (the sticky behavior). Same results as embi_re_search otherwise, including
 * the start <= in.len precondition and the u/v-mode surrogate pair adjustment of start. */
embi_re_status embi_re_match_at(const embi_re_prog *prog, embi_re_input in, uint32_t start,
                                uint32_t *caps, const embi_re_limits *limits);
