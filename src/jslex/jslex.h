#pragma once
/*
 * ES2025 lexer. Internal to the engine: cross-module symbols use the embi_ prefix.
 *
 * Input is a UTF-8 buffer. Invalid sequences decode to U+FFFD, as embi_utf8_decode does.
 * Leniency: the three-byte WTF-8 form of a lone surrogate (ED A0..BF xx) is accepted in every source
 * and yields that surrogate (source built by eval/Function from a JS string needs it); a raw file
 * with CESU-8 or surrogate bytes therefore gives lone surrogates, not U+FFFD.
 * The lexer allocates nothing and holds no heap state.
 *
 * Cooked values of STRING, TEMPLATE and escaped NAME tokens are UTF-16 code units written into
 * a caller-provided scratch buffer. A scan overwrites the buffer, so only the most recent token's
 * cooked value is valid. A token needs at most as many units as its byte length, so a scratch
 * of at least the source length always suffices. A token that does not fit is an ERROR token.
 *
 * The lexer state is a plain struct. A parser saves a position by copying it and restores it by
 * copying it back. Scratch contents are then rebuilt by the next scan.
 *
 * Goal-dependent scanning is driven by the parser:
 *   - Set lx.regex_ok = true right before embi_jslex_next() when a primary expression may start.
 *     A '/' or '/=' then begins a regular expression literal. The flag covers that one call only.
 *   - When the parser reads the RBRACE that closes a template substitution, call
 *     embi_jslex_rescan_template() on that token to read the TPL_MIDDLE or TPL_TAIL part.
 *   - embi_jslex_rescan_regex() re-scans a DIV or DIV_EQ token as a regular expression literal.
 */
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ lexer flags */

/* Module goal: HTML-like comments (<!-- and -->) are not recognized. */
#define EMBI_JSLEX_MODULE 0x1u

/* ------------------------------------------------------------------ token kinds */

typedef enum embi_jstok_kind : uint8_t {
    EMBI_JST_EOF,
    EMBI_JST_ERROR,        /* lx.err holds the message, lx.err_pos the offset */
    EMBI_JST_NAME,         /* IdentifierName, keywords included (see kw) */
    EMBI_JST_PRIVATE_NAME, /* #name. The name is the source span [start + 1, end) */
    EMBI_JST_NUMBER,       /* num, radix */
    EMBI_JST_BIGINT,       /* digits via embi_jslex_bigint_digits, radix */
    EMBI_JST_STRING,       /* cooked */
    EMBI_JST_REGEX,        /* body is [start + 1, aux_end), flags are [aux_end + 1, end) */
    EMBI_JST_TPL_NOSUB,    /* `...`. Raw text is [start + 1, aux_end) */
    EMBI_JST_TPL_HEAD,     /* `...${`. Raw text is [start + 1, aux_end) */
    EMBI_JST_TPL_MIDDLE,   /* }...${. Raw text is [start + 1, aux_end) */
    EMBI_JST_TPL_TAIL,     /* }...`. Raw text is [start + 1, aux_end) */
    EMBI_JST_LBRACE,       /* { */
    EMBI_JST_RBRACE,       /* } */
    EMBI_JST_LPAREN,
    EMBI_JST_RPAREN,
    EMBI_JST_LBRACK,
    EMBI_JST_RBRACK,
    EMBI_JST_DOT,
    EMBI_JST_ELLIPSIS,     /* ... */
    EMBI_JST_SEMI,
    EMBI_JST_COMMA,
    EMBI_JST_LT,           /* < */
    EMBI_JST_GT,           /* > */
    EMBI_JST_LE,           /* <= */
    EMBI_JST_GE,           /* >= */
    EMBI_JST_EQ,           /* == */
    EMBI_JST_NE,           /* != */
    EMBI_JST_SEQ,          /* === */
    EMBI_JST_SNE,          /* !== */
    EMBI_JST_ADD,
    EMBI_JST_SUB,
    EMBI_JST_MUL,
    EMBI_JST_MOD,
    EMBI_JST_EXP,          /* ** */
    EMBI_JST_INC,          /* ++ */
    EMBI_JST_DEC,          /* -- */
    EMBI_JST_SHL,          /* << */
    EMBI_JST_SAR,          /* >> */
    EMBI_JST_SHR,          /* >>> */
    EMBI_JST_BAND,         /* & */
    EMBI_JST_BOR,          /* | */
    EMBI_JST_BXOR,         /* ^ */
    EMBI_JST_NOT,          /* ! */
    EMBI_JST_BNOT,         /* ~ */
    EMBI_JST_LAND,         /* && */
    EMBI_JST_LOR,          /* || */
    EMBI_JST_NULLISH,      /* ?? */
    EMBI_JST_QUESTION,     /* ? */
    EMBI_JST_OPTCHAIN,     /* ?. (not followed by a digit) */
    EMBI_JST_COLON,
    EMBI_JST_ASSIGN,       /* = */
    EMBI_JST_ADD_EQ,
    EMBI_JST_SUB_EQ,
    EMBI_JST_MUL_EQ,
    EMBI_JST_MOD_EQ,
    EMBI_JST_EXP_EQ,       /* **= */
    EMBI_JST_SHL_EQ,
    EMBI_JST_SAR_EQ,
    EMBI_JST_SHR_EQ,
    EMBI_JST_BAND_EQ,
    EMBI_JST_BOR_EQ,
    EMBI_JST_BXOR_EQ,
    EMBI_JST_LAND_EQ,      /* &&= */
    EMBI_JST_LOR_EQ,       /* ||= */
    EMBI_JST_NULLISH_EQ,   /* ??= */
    EMBI_JST_ARROW,        /* => */
    EMBI_JST_DIV,          /* / (when a regex is not allowed) */
    EMBI_JST_DIV_EQ,       /* /= (when a regex is not allowed) */
} embi_jstok_kind;

/* ------------------------------------------------------------------ token flags */

#define EMBI_JST_F_NL_BEFORE  0x01u /* a line terminator precedes the token (also inside comments) */
#define EMBI_JST_F_ESCAPED    0x02u /* NAME or PRIVATE_NAME is spelled with \u escapes */
#define EMBI_JST_F_LEGACY     0x04u /* legacy octal or NonOctalDecimal number, or a string with a
                                       legacy octal escape or \8 \9 (strict mode error) */
#define EMBI_JST_F_BAD_ESCAPE 0x08u /* template with an invalid escape; cooked is nullptr */

/* ------------------------------------------------------------------ keywords */

/* Reserved words, strict-mode reserved words and the contextual words the parser checks.
 * The lexer reports every NAME; the parser decides when a word is reserved. */
#define EMBI_JSKW_LIST(X)                                                                      \
    X(break) X(case) X(catch) X(class) X(const) X(continue) X(debugger) X(default)             \
    X(delete) X(do) X(else) X(enum) X(export) X(extends) X(false) X(finally) X(for)            \
    X(function) X(if) X(import) X(in) X(instanceof) X(new) X(null) X(return) X(super)          \
    X(switch) X(this) X(throw) X(true) X(try) X(typeof) X(var) X(void) X(while) X(with)        \
    X(implements) X(interface) X(let) X(package) X(private) X(protected) X(public) X(static)    \
    X(yield) X(await) X(async) X(of) X(get) X(set) X(target) X(meta) X(from) X(as)

#define EMBI_JSKW_ENUM(k) EMBI_JSKW_##k,
typedef enum embi_jskw : uint8_t {
    EMBI_JSKW_NONE = 0,
    EMBI_JSKW_LIST(EMBI_JSKW_ENUM)
} embi_jskw;
#undef EMBI_JSKW_ENUM

/* Returns the keyword spelled by the len ASCII bytes at p, or EMBI_JSKW_NONE. */
embi_jskw embi_jslex_keyword(const uint8_t *p, uint32_t len);

/* ------------------------------------------------------------------ unicode identifiers */

/* Unicode ID_Start and ID_Continue (UCD 17.0.0). '$' is not included: the lexer adds it,
 * since ECMAScript allows it in every identifier position. */
bool embi_jslex_is_id_start(uint32_t cp);
bool embi_jslex_is_id_continue(uint32_t cp);

/* ------------------------------------------------------------------ tokens and lexer */

typedef struct embi_jstok {
    embi_jstok_kind kind;
    embi_jskw kw;         /* NAME: keyword of the (cooked) spelling, else EMBI_JSKW_NONE */
    uint8_t radix;        /* NUMBER and BIGINT: 2, 8, 10 or 16 */
    uint8_t flags;        /* EMBI_JST_F_* */
    uint32_t start;       /* byte offset of the first byte */
    uint32_t end;         /* byte offset after the last byte */
    uint32_t aux_end;     /* REGEX: end of body. Templates: end of raw text */
    uint32_t line;        /* 1-based line of start */
    uint32_t col;         /* 1-based column of start, in UTF-16 code units from the line start */
    double num;           /* NUMBER value (correctly rounded) */
    const uint16_t *cooked; /* STRING, TEMPLATE, escaped NAME: UTF-16 scratch view */
    uint32_t cooked_len;
} embi_jstok;

typedef struct embi_jslex {
    const uint8_t *src;
    uint32_t len;
    uint32_t pos;          /* where the next scan starts */
    uint32_t flags;        /* EMBI_JSLEX_* */
    uint16_t *scratch;
    uint32_t scratch_cap;  /* in UTF-16 code units */
    bool regex_ok;         /* applies to the next embi_jslex_next() call only */
    bool skim;             /* lookahead scans: tokens carry no line or column, and the cooked value of a
                              string is not built (the parser reads only kinds, flags and offsets) */
    bool failed;           /* an error was reported; every later call returns ERROR */
    const char *err;       /* message of the last error */
    uint32_t err_pos;      /* byte offset of the last error */
    /* Line and column cache, advanced monotonically. Callers do not touch it. */
    uint32_t pc_pos;
    uint32_t pc_line;
    uint32_t pc_col;
} embi_jslex;

void embi_jslex_init(embi_jslex *lx, const uint8_t *src, uint32_t len, uint16_t *scratch,
                     uint32_t scratch_cap, uint32_t flags);

/* Scans the next token into t. Whitespace and comments are skipped and reported through
 * EMBI_JST_F_NL_BEFORE. */
void embi_jslex_next(embi_jslex *lx, embi_jstok *t);

/* t must be a DIV or DIV_EQ token. Re-scans it from t->start as a regular expression literal. */
void embi_jslex_rescan_regex(embi_jslex *lx, embi_jstok *t);

/* t must be an RBRACE token that closes a template substitution. Re-scans the template text
 * after it as TPL_MIDDLE or TPL_TAIL. */
void embi_jslex_rescan_template(embi_jslex *lx, embi_jstok *t);

/* Writes the raw text of a template token as UTF-16 with CR and CRLF normalized to LF. Returns
 * the number of units needed. Writes at most cap units. */
uint32_t embi_jslex_raw16(const embi_jslex *lx, const embi_jstok *t, uint16_t *out, uint32_t cap);

/* Writes the digits of a BIGINT token as ASCII, without the radix prefix, separators or the
 * 'n' suffix. Returns the number of digits needed. Writes at most cap bytes. */
uint32_t embi_jslex_bigint_digits(const embi_jslex *lx, const embi_jstok *t, char *out,
                                  uint32_t cap);

/* ------------------------------------------------------------------ numeric conversion */

/* Correctly rounded (round-half-even) conversion of decimal literal text, with no separators:
 * digits, an optional '.', and an optional exponent. Used by the lexer and by tests. */
double embi_jslex_decimal_to_double(const char *s, uint32_t len);

/* Correctly rounded conversion of digits in the given radix (2, 8, 10 or 16). */
double embi_jslex_radix_to_double(const char *s, uint32_t len, unsigned radix);
