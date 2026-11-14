/*
 * JSON: the global JSON object.
 *
 * parse is a strict ECMA-404 parser (no extensions) that builds objects with CreateDataProperty,
 * then runs the reviver walk of InternalizeJSONProperty. stringify is SerializeJSONProperty with
 * a replacer function or property list, the gap, toJSON and cycle detection. Output goes into one
 * UTF-16 buffer on the C heap. The buffer and the scratch vectors hold no cell references, so the
 * collector never needs to see them. Objects that are on the serializer's cycle stack are also
 * held by a live C frame, so the conservative stack scan keeps them alive.
 *
 * Recursion (parse, reviver walk, serialize) checks the native stack budget at every level and
 * throws a RangeError when it is spent. Deep nesting therefore never overflows the C stack.
 */
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"
#include "num/num.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define JSON_GAP_MAX 10u

typedef EMBI_VEC(uint16_t) u16_vec;
typedef EMBI_VEC(char) char_vec;
typedef EMBI_VEC(embi_js_object *) obj_vec;

static embi_js_value throw_stack(embi_js_runtime *rt)
{
    return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
}

/* The key of array index i. ToPropertyKey covers the indices above 2^32 - 2. */
static embi_js_key index_key(embi_js_runtime *rt, double i)
{
    if (i <= (double)EMBI_JS_MAX_ARRAY_INDEX) {
        return embi_js_key_from_index((uint32_t)i);
    }
    return embi_js_to_property_key(rt, embi_js_number(i));
}

/* ------------------------------------------------------------------ parse */

/* A parse record (json-parse-with-source, built only when a reviver is given): the value of one
 * node, the source span of a primitive, and the records of its elements or entries in source
 * order. For an object, map gives each key the index of its last entry. */
typedef struct jnode {
    embi_js_value value;
    uint32_t start;
    uint32_t end;
    bool obj;
    bool arr;
    EMBI_VEC(struct jnode *) kids;
    embi_hashmap map;
} jnode;

typedef EMBI_VEC(jnode *) node_vec;

typedef struct parser {
    embi_js_runtime *rt;
    const embi_js_string *src;
    uint32_t pos;
    uint32_t len;
    u16_vec str;  /* scratch: decoded contents of the string being scanned */
    char_vec num; /* scratch: the text of the number being scanned */
    bool track;   /* build the parse records */
    embi_js_varray *keep; /* every value in the records: a GC root while they are in use */
    uint32_t keep_len;
    node_vec nodes; /* every record, freed by parser_free */
} parser;

/* Keeps a value of a parse record alive until the parser is freed. */
static void keep_push(parser *p, embi_js_value v)
{
    if (!p->track || !embi_js_is_cell(v)) {
        return;
    }
    if (p->keep_len == p->keep->hdr.aux) {
        p->keep = embi_js_varray_grow(p->rt, p->keep, p->keep_len * 2u + 8u);
    }
    EMBI_JS_STORE(p->keep, p->keep->elems[p->keep_len++], v);
}

/* A new record for a value whose source starts at start and ends at the current position. */
static jnode *node_new(parser *p, embi_js_value v, uint32_t start)
{
    jnode *n = embi_alloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->value = v;
    n->start = start;
    n->end = p->pos;
    embi_vec_push(&p->nodes, n);
    keep_push(p, v);
    return n;
}

/* The next code unit, or -1 at the end of the input. */
static int peek(const parser *p)
{
    return p->pos < p->len ? (int)embi_js_str_at(p->src, p->pos) : -1;
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void skip_ws(parser *p)
{
    while (p->pos < p->len) {
        int c = (int)embi_js_str_at(p->src, p->pos);
        if (c != 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) {
            break;
        }
        p->pos++;
    }
}

static embi_js_value unexpected(parser *p)
{
    if (p->pos >= p->len) {
        return embi_js_throw_syntax_error(p->rt, "Unexpected end of JSON input");
    }
    return embi_js_throw_syntax_error(p->rt, "Unexpected token in JSON at position %u",
                                      (unsigned)p->pos);
}

static void syntax_at(parser *p, const char *what)
{
    (void)embi_js_throw_syntax_error(p->rt, "%s in JSON at position %u", what, (unsigned)p->pos);
}

/* A string literal at the opening quote p->pos with no escape and no control character (J5m2):
 * its text is the source between the quotes, so it can be sliced without the scratch buffer.
 * Sets *end to the index of the closing quote. */
static bool plain_string(const parser *p, uint32_t *end)
{
    for (uint32_t i = p->pos + 1; i < p->len; i++) {
        uint16_t c = embi_js_str_at(p->src, i);
        if (c == '"') {
            *end = i;
            return true;
        }
        if (c == '\\' || c < 0x20) {
            return false;
        }
    }
    return false;
}

/* Scans a string literal at the opening quote into p->str. Returns false with the exception
 * pending. Lone surrogates are kept as they are. */
static bool scan_string(parser *p)
{
    p->pos++;
    p->str.len = 0;
    for (;;) {
        int c = peek(p);
        if (c < 0) {
            (void)unexpected(p);
            return false;
        }
        p->pos++;
        if (c == '"') {
            return true;
        }
        if (c < 0x20) {
            syntax_at(p, "Bad control character in string literal");
            return false;
        }
        if (c != '\\') {
            embi_vec_push(&p->str, (uint16_t)c);
            continue;
        }
        int e = peek(p);
        if (e < 0) {
            (void)unexpected(p);
            return false;
        }
        p->pos++;
        uint16_t out;
        switch (e) {
        case '"':
            out = '"';
            break;
        case '\\':
            out = '\\';
            break;
        case '/':
            out = '/';
            break;
        case 'b':
            out = '\b';
            break;
        case 'f':
            out = '\f';
            break;
        case 'n':
            out = '\n';
            break;
        case 'r':
            out = '\r';
            break;
        case 't':
            out = '\t';
            break;
        case 'u': {
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int h = peek(p);
                int d = hex_value(h);
                if (d < 0) {
                    if (h < 0) {
                        (void)unexpected(p);
                    } else {
                        syntax_at(p, "Bad Unicode escape");
                    }
                    return false;
                }
                v = v * 16u + (uint32_t)d;
                p->pos++;
            }
            out = (uint16_t)v;
            break;
        }
        default:
            syntax_at(p, "Bad escaped character");
            return false;
        }
        embi_vec_push(&p->str, out);
    }
}

static embi_js_value parse_number(parser *p)
{
    uint32_t start = p->pos;
    if (peek(p) == '-') {
        p->pos++;
    }
    uint32_t digits = p->pos;
    bool simple = true; /* no fraction and no exponent (J5m2) */
    int c = peek(p);
    if (c == '0') {
        p->pos++;
    } else if (c >= '1' && c <= '9') {
        while (is_digit(peek(p))) {
            p->pos++;
        }
    } else {
        return unexpected(p);
    }
    if (peek(p) == '.') {
        simple = false;
        p->pos++;
        if (!is_digit(peek(p))) {
            return unexpected(p);
        }
        while (is_digit(peek(p))) {
            p->pos++;
        }
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        simple = false;
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') {
            p->pos++;
        }
        if (!is_digit(peek(p))) {
            return unexpected(p);
        }
        while (is_digit(peek(p))) {
            p->pos++;
        }
    }
    if (simple && p->pos - digits <= 15u) {
        /* At most 15 digits are exact in a double, so the value is the digit sum (J5m2). A
         * negative zero keeps its sign. */
        double v = 0.0;
        for (uint32_t i = digits; i < p->pos; i++) {
            v = v * 10.0 + (double)(embi_js_str_at(p->src, i) - '0');
        }
        return embi_js_number(digits != start ? -v : v);
    }
    p->num.len = 0;
    for (uint32_t i = start; i < p->pos; i++) {
        embi_vec_push(&p->num, (char)embi_js_str_at(p->src, i));
    }
    size_t used = 0;
    double d = embi_num_parse_decimal(p->num.data, p->num.len, &used);
    return embi_js_number(d);
}

static embi_js_value parse_literal(parser *p, const char *word, embi_js_value v)
{
    for (size_t i = 0; word[i] != '\0'; i++) {
        if (peek(p) != (int)(unsigned char)word[i]) {
            return unexpected(p);
        }
        p->pos++;
    }
    return v;
}

static embi_js_value parse_value(parser *p, jnode **out);

/* The value of a primitive token at the current position. */
static embi_js_value parse_scalar(parser *p)
{
    switch (peek(p)) {
    case '"': {
        uint32_t end = 0;
        if (plain_string(p, &end)) {
            embi_js_string *s = embi_js_str_substring(p->rt, p->src, p->pos + 1u, end);
            p->pos = end + 1u;
            return embi_js_from_string(s);
        }
        if (!scan_string(p)) {
            return EXC;
        }
        embi_js_string *s = embi_js_str_new_utf16(p->rt, p->str.data, p->str.len);
        return embi_js_from_string(s);
    }
    case 't':
        return parse_literal(p, "true", EMBI_JS_TRUE);
    case 'f':
        return parse_literal(p, "false", EMBI_JS_FALSE);
    case 'n':
        return parse_literal(p, "null", EMBI_JS_NULL);
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        return parse_number(p);
    default:
        return unexpected(p);
    }
}

static embi_js_value parse_object(parser *p, jnode **out)
{
    embi_js_runtime *rt = p->rt;
    if (!embi_js_native_stack_ok(rt)) {
        return throw_stack(rt);
    }
    uint32_t start = p->pos;
    p->pos++;
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_value ov = embi_js_from_object(o);
    jnode *n = out ? node_new(p, ov, start) : nullptr;
    if (n) {
        n->obj = true;
    }
    skip_ws(p);
    if (peek(p) == '}') {
        p->pos++;
        if (out) {
            *out = n;
        }
        return ov;
    }
    for (;;) {
        skip_ws(p);
        if (peek(p) != '"') {
            return unexpected(p);
        }
        embi_js_string *text = nullptr;
        uint32_t kend = 0;
        if (plain_string(p, &kend)) {
            text = embi_js_str_substring(rt, p->src, p->pos + 1u, kend);
            p->pos = kend + 1u;
        } else {
            if (!scan_string(p)) {
                return EXC;
            }
            text = embi_js_str_new_utf16(rt, p->str.data, p->str.len);
        }
        embi_js_key k = embi_js_key_from_str(rt, text);
        skip_ws(p);
        if (peek(p) != ':') {
            return unexpected(p);
        }
        p->pos++;
        skip_ws(p);
        jnode *child = nullptr;
        embi_js_value v = parse_value(p, n ? &child : nullptr);
        if (v == EXC) {
            return EXC;
        }
        /* A new named key of a plain object is appended to its shape (J5m2). A repeated key or an
         * index key takes the generic CreateDataProperty. */
        if (!embi_js_key_is_index(k) && embi_js_obj_shape_only(o) &&
            embi_js_shape_find(o->shape, k) == nullptr) {
            embi_js_define_fresh(rt, o, k, v, EMBI_JS_ATTR_DEFAULT);
        } else if (embi_js_create_data_property(rt, o, k, v) == EMBI_JS_THREW) {
            return EXC;
        }
        if (n) {
            /* A repeated key keeps its last entry, as the object does. */
            embi_hashmap_put(&n->map, k, n->kids.len);
            embi_vec_push(&n->kids, child);
        }
        skip_ws(p);
        int c = peek(p);
        if (c == ',') {
            p->pos++;
            continue;
        }
        if (c == '}') {
            p->pos++;
            if (out) {
                *out = n;
            }
            return ov;
        }
        return unexpected(p);
    }
}

static embi_js_value parse_array(parser *p, jnode **out)
{
    embi_js_runtime *rt = p->rt;
    if (!embi_js_native_stack_ok(rt)) {
        return throw_stack(rt);
    }
    uint32_t start = p->pos;
    p->pos++;
    embi_js_object *a = embi_js_array_create_default(rt, 0.0);
    embi_js_value av = embi_js_from_object(a);
    jnode *n = out ? node_new(p, av, start) : nullptr;
    if (n) {
        n->arr = true;
    }
    skip_ws(p);
    if (peek(p) == ']') {
        p->pos++;
        if (out) {
            *out = n;
        }
        return av;
    }
    uint32_t idx = 0;
    for (;;) {
        skip_ws(p);
        jnode *child = nullptr;
        embi_js_value v = parse_value(p, n ? &child : nullptr);
        if (v == EXC) {
            return EXC;
        }
        if (embi_js_create_data_property(rt, a, embi_js_key_from_index(idx), v) ==
            EMBI_JS_THREW) {
            return EXC;
        }
        if (n) {
            embi_vec_push(&n->kids, child);
        }
        idx++;
        skip_ws(p);
        int c = peek(p);
        if (c == ',') {
            p->pos++;
            continue;
        }
        if (c == ']') {
            p->pos++;
            if (out) {
                *out = n;
            }
            return av;
        }
        return unexpected(p);
    }
}

static embi_js_value parse_value(parser *p, jnode **out)
{
    int c = peek(p);
    if (c == '{') {
        return parse_object(p, out);
    }
    if (c == '[') {
        return parse_array(p, out);
    }
    uint32_t start = p->pos;
    embi_js_value v = parse_scalar(p);
    if (v != EXC && out) {
        *out = node_new(p, v, start);
    }
    return v;
}

/* Parses the whole input as one JSON value. Leading and trailing white space is skipped. out
 * receives the record tree when the parser tracks records. */
static embi_js_value parse_run(parser *p, jnode **out)
{
    skip_ws(p);
    embi_js_value v = parse_value(p, out);
    if (v != EXC) {
        skip_ws(p);
        if (p->pos < p->len) {
            v = unexpected(p);
        }
    }
    return v;
}

static void parser_init(parser *p, embi_js_runtime *rt, const embi_js_string *src, bool track)
{
    memset(p, 0, sizeof *p);
    p->rt = rt;
    p->src = src;
    p->len = embi_js_str_len(src);
    p->track = track;
    if (track) {
        p->keep = embi_js_varray_new(rt, 8);
    }
}

static void parser_free(parser *p)
{
    embi_vec_free(&p->str);
    embi_vec_free(&p->num);
    for (uint32_t i = 0; i < p->nodes.len; i++) {
        jnode *n = p->nodes.data[i];
        embi_vec_free(&n->kids);
        embi_hashmap_free(&n->map);
        embi_free(n, sizeof *n);
    }
    embi_vec_free(&p->nodes);
    p->keep = nullptr;
}

static embi_js_value parse_text(embi_js_runtime *rt, const embi_js_string *src)
{
    parser p;
    parser_init(&p, rt, src, false);
    embi_js_value v = parse_run(&p, nullptr);
    parser_free(&p);
    return v;
}

/* The intrinsic parse of JSON text (module code uses it for JSON modules). */
embi_js_value embi_js_json_parse_text(embi_js_runtime *rt, const embi_js_string *src)
{
    return parse_text(rt, src);
}

/* ------------------------------------------------------------------ reviver */

/* InternalizeJSONProperty with the source text context (json-parse-with-source). rec is the parse
 * record of the value that the holder's property had when it was parsed, or nullptr. It is used
 * only while the value is still the same (SameValue), and it supplies context.source for a
 * primitive. */
typedef struct walk {
    embi_js_realm *r;
    embi_js_value reviver;
    const embi_js_string *text;
} walk;

static embi_js_value internalize(walk *w, embi_js_object *holder, embi_js_key name,
                                 const jnode *rec);

/* The record of the element at index i of an array record, or nullptr. */
static const jnode *elem_rec(const jnode *rec, double i)
{
    if (!rec || !rec->arr || i >= (double)rec->kids.len) {
        return nullptr;
    }
    return rec->kids.data[(uint32_t)i];
}

/* The record of the entry with key k in an object record, or nullptr. */
static const jnode *entry_rec(const jnode *rec, embi_js_key k)
{
    uint64_t idx = 0;
    if (!rec || !rec->obj || !embi_hashmap_get(&rec->map, k, &idx)) {
        return nullptr;
    }
    return rec->kids.data[idx];
}

/* Applies the reviver to one member and stores or deletes the result. */
static bool revive_member(walk *w, embi_js_object *o, embi_js_key k, const jnode *rec)
{
    embi_js_runtime *rt = w->r->rt;
    embi_js_value nv = internalize(w, o, k, rec);
    if (nv == EXC) {
        return false;
    }
    if (embi_js_is_undefined(nv)) {
        return embi_js_obj_delete(rt, o, k) != EMBI_JS_THREW;
    }
    return embi_js_create_data_property(rt, o, k, nv) != EMBI_JS_THREW;
}

/* A copy of the source text between two offsets, as a string. */
static embi_js_string *source_text(embi_js_runtime *rt, const embi_js_string *text, uint32_t start,
                                   uint32_t end)
{
    uint32_t n = end - start;
    uint16_t *buf = embi_alloc((size_t)n * sizeof *buf);
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = embi_js_str_at(text, start + i);
    }
    embi_js_string *s = embi_js_str_new_utf16(rt, buf, n);
    embi_free(buf, (size_t)n * sizeof *buf);
    return s;
}

static embi_js_value internalize(walk *w, embi_js_object *holder, embi_js_key name,
                                 const jnode *rec)
{
    embi_js_runtime *rt = w->r->rt;
    if (!embi_js_native_stack_ok(rt)) {
        return throw_stack(rt);
    }
    embi_js_value val = embi_js_obj_get(rt, holder, name, embi_js_from_object(holder));
    if (val == EXC) {
        return EXC;
    }
    if (rec && !embi_js_same_value(rec->value, val)) {
        rec = nullptr;
    }
    embi_js_object *ctx = embi_js_object_new_plain(rt);
    embi_js_value ctx_v = embi_js_from_object(ctx);
    if (rec && !embi_js_is_object(val)) {
        embi_js_string *src = source_text(rt, w->text, rec->start, rec->end);
        if (embi_js_create_data_property(rt, ctx, embi_js_key_c(rt, "source"),
                                         embi_js_from_string(src)) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    if (embi_js_is_object(val)) {
        embi_js_object *o = embi_js_as_object(val);
        int val_is_arr = embi_js_is_array_checked(rt, val);
        if (val_is_arr == EMBI_JS_THREW) {
            return EXC;
        }
        if (val_is_arr == EMBI_JS_YES) {
            double len;
            if (!embi_js_length_of_array_like(rt, o, &len)) {
                return EXC;
            }
            for (double i = 0.0; i < len; i++) {
                embi_js_key k = index_key(rt, i);
                if (k == EXC || !revive_member(w, o, k, elem_rec(rec, i))) {
                    return EXC;
                }
            }
        } else {
            embi_js_key_vec keys = {nullptr, 0, 0};
            if (embi_js_enumerable_own_keys(rt, o, &keys) == EMBI_JS_THREW) {
                embi_vec_free(&keys);
                return EXC;
            }
            for (uint32_t i = 0; i < keys.len; i++) {
                if (!revive_member(w, o, keys.data[i], entry_rec(rec, keys.data[i]))) {
                    embi_vec_free(&keys);
                    return EXC;
                }
            }
            embi_vec_free(&keys);
        }
    }
    embi_js_value args[3] = {embi_js_key_to_value(rt, name), val, ctx_v};
    return embi_js_call(rt, w->reviver, embi_js_from_object(holder), 3, args);
}

static embi_js_value json_parse(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value text_v = argc > 0 ? argv[0] : UNDEF;
    embi_js_value reviver = argc > 1 ? argv[1] : UNDEF;

    embi_js_string *text = embi_js_to_string(rt, text_v);
    if (!text) {
        return EXC;
    }
    bool revive = embi_js_is_callable(reviver);
    parser p;
    parser_init(&p, rt, text, revive);
    jnode *root = nullptr;
    embi_js_value unfiltered = parse_run(&p, revive ? &root : nullptr);
    if (unfiltered == EXC || !revive) {
        parser_free(&p);
        return unfiltered;
    }
    embi_js_object *holder = embi_js_object_new_plain(rt);
    embi_js_key empty = embi_js_key_c(rt, "");
    if (embi_js_create_data_property_or_throw(rt, holder, empty, unfiltered) != EMBI_JS_YES) {
        parser_free(&p);
        return EXC;
    }
    walk w = {r, reviver, text};
    embi_js_value res = internalize(&w, holder, empty, root);
    parser_free(&p);
    return res;
}

/* ------------------------------------------------------------------ raw JSON (ES2026) */

/* The objects of JSON.rawJSON: a null-prototype object with the [[IsRawJSON]] slot, which is this
 * class. */
static const embi_js_class class_raw_json = {.name = "Object"};

static bool is_json_ws(uint16_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* JSON.rawJSON(text): text must be a JSON primitive, with no white space at either end. */
static embi_js_value json_raw_json(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : UNDEF);
    if (!s) {
        return EXC;
    }
    uint32_t len = embi_js_str_len(s);
    if (len == 0 || is_json_ws(embi_js_str_at(s, 0)) || is_json_ws(embi_js_str_at(s, len - 1))) {
        return embi_js_throw_syntax_error(rt, "JSON.rawJSON: invalid JSON text");
    }
    uint16_t first = embi_js_str_at(s, 0);
    if (first == '{' || first == '[') {
        return embi_js_throw_syntax_error(rt, "JSON.rawJSON: text must be a JSON primitive");
    }
    parser p;
    parser_init(&p, rt, s, false);
    embi_js_value v = parse_run(&p, nullptr);
    parser_free(&p);
    if (v == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_raw_json);
    embi_js_define_raw(rt, o, embi_js_key_c(rt, "rawJSON"), embi_js_from_string(s),
                       EMBI_JS_ATTR_ENUMERABLE);
    (void)embi_js_obj_prevent_extensions(rt, o);
    return embi_js_from_object(o);
}

static embi_js_value json_is_raw_json(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)magic;
    embi_js_value v = argc > 0 ? argv[0] : UNDEF;
    return embi_js_from_bool(embi_js_is_object(v) &&
                             embi_js_as_object(v)->klass == &class_raw_json);
}

/* ------------------------------------------------------------------ stringify */

typedef struct ser {
    embi_js_runtime *rt;
    embi_js_key tojson_key; /* the atom "toJSON", interned once per call (atoms are permanent) */
    embi_js_value fn;       /* replacer function, or UNDEFINED */
    embi_js_key_vec props;  /* property list from an array replacer (keys, no cells) */
    bool has_props;
    uint16_t gap[JSON_GAP_MAX];
    uint32_t gap_len;
    obj_vec stack;          /* objects being serialized (cycle detection) */
    u16_vec out;
    bool overflow;          /* the output passed the maximum string length */
} ser;

static void ser_free(ser *s)
{
    embi_vec_free(&s->props);
    embi_vec_free(&s->stack);
    embi_vec_free(&s->out);
}

static void put(ser *s, uint16_t u)
{
    if (s->out.len >= EMBI_JS_STR_MAX_LEN) {
        s->overflow = true;
        return;
    }
    embi_vec_push(&s->out, u);
}

static void put_ascii(ser *s, const char *t)
{
    for (; *t != '\0'; t++) {
        put(s, (uint16_t)(unsigned char)*t);
    }
}

/* A newline followed by level copies of the gap. */
static void newline_indent(ser *s, uint32_t level)
{
    put(s, '\n');
    for (uint32_t i = 0; i < level; i++) {
        for (uint32_t j = 0; j < s->gap_len; j++) {
            put(s, s->gap[j]);
        }
    }
}

static void put_u_escape(ser *s, uint16_t c)
{
    static const char hex[] = "0123456789abcdef";
    put(s, '\\');
    put(s, 'u');
    for (int sh = 12; sh >= 0; sh -= 4) {
        put(s, (uint16_t)(unsigned char)hex[(c >> sh) & 0xFu]);
    }
}

/* QuoteJSONString. Lone surrogates are escaped, so the output is well-formed Unicode. */
static uint16_t unit_at(const uint8_t *b8, const uint16_t *b16, uint32_t i)
{
    return b8 ? (uint16_t)b8[i] : b16[i];
}

static void quote(ser *s, const embi_js_string *str)
{
    uint32_t n = embi_js_str_len(str);
    /* The string is flat, so its units are read from storage directly (J5m2). */
    const uint8_t *b8 = embi_js_str_is_wide(str) ? nullptr : embi_js_str_latin1(str);
    const uint16_t *b16 = b8 ? nullptr : embi_js_str_wide(str);
    put(s, '"');
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = unit_at(b8, b16, i);
        switch (c) {
        case '"':
            put(s, '\\');
            put(s, '"');
            break;
        case '\\':
            put(s, '\\');
            put(s, '\\');
            break;
        case '\b':
            put(s, '\\');
            put(s, 'b');
            break;
        case '\f':
            put(s, '\\');
            put(s, 'f');
            break;
        case '\n':
            put(s, '\\');
            put(s, 'n');
            break;
        case '\r':
            put(s, '\\');
            put(s, 'r');
            break;
        case '\t':
            put(s, '\\');
            put(s, 't');
            break;
        default: {
            bool paired = false;
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
                uint16_t d = unit_at(b8, b16, i + 1);
                if (d >= 0xDC00 && d <= 0xDFFF) {
                    put(s, c);
                    put(s, d);
                    i++;
                    paired = true;
                }
            }
            if (!paired) {
                if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) {
                    put_u_escape(s, c);
                } else {
                    put(s, c);
                }
            }
            break;
        }
        }
    }
    put(s, '"');
}

static void put_number(ser *s, embi_js_value v)
{
    if (embi_js_is_int32(v)) {
        int32_t i = embi_js_as_int32(v);
        uint32_t u = i < 0 ? 0u - (uint32_t)i : (uint32_t)i;
        char tmp[12];
        int n = 0;
        if (i < 0) {
            put(s, '-');
        }
        do {
            tmp[n++] = (char)('0' + (char)(u % 10u));
            u /= 10u;
        } while (u != 0);
        while (n > 0) {
            put(s, (uint16_t)(unsigned char)tmp[--n]);
        }
        return;
    }
    embi_js_string *str = embi_js_number_to_str(s->rt, embi_js_as_double(v));
    if (str) {
        uint32_t n = embi_js_str_len(str);
        for (uint32_t i = 0; i < n; i++) {
            put(s, embi_js_str_at(str, i));
        }
    }
}

/* Reports a pending overflow as a RangeError. */
static int overflow_error(ser *s)
{
    (void)embi_js_throw_range_error(s->rt, "Invalid string length");
    return EMBI_JS_THREW;
}

/* Pushes o on the cycle stack. Returns false with the exception pending on a cycle or when the
 * native stack is spent. */
static bool enter(ser *s, embi_js_object *o)
{
    embi_js_runtime *rt = s->rt;
    if (!embi_js_native_stack_ok(rt)) {
        (void)throw_stack(rt);
        return false;
    }
    for (uint32_t i = 0; i < s->stack.len; i++) {
        if (s->stack.data[i] == o) {
            (void)embi_js_throw_type_error(rt, "Converting circular structure to JSON");
            return false;
        }
    }
    embi_vec_push(&s->stack, o);
    return true;
}

static int ser_property(ser *s, embi_js_key key, embi_js_object *holder);

/* SerializeJSONObject. Writes nothing and returns THREW when an exception is pending. */
static int ser_object(ser *s, embi_js_object *o)
{
    embi_js_runtime *rt = s->rt;
    if (!enter(s, o)) {
        return EMBI_JS_THREW;
    }
    embi_js_key_vec own = {nullptr, 0, 0};
    const embi_js_key_vec *list = &s->props;
    if (!s->has_props) {
        if (embi_js_obj_shape_only(o)) {
            /* The keys come from the shape, which is the own-key order (J5m2). */
            embi_js_obj_enum_string_keys(o, &own);
        } else if (embi_js_enumerable_own_keys(rt, o, &own) == EMBI_JS_THREW) {
            embi_vec_free(&own);
            s->stack.len--;
            return EMBI_JS_THREW;
        }
        list = &own;
    }
    uint32_t level = s->stack.len;
    int result = EMBI_JS_YES;
    bool any = false;
    put(s, '{');
    for (uint32_t i = 0; i < list->len; i++) {
        if (s->overflow) {
            result = overflow_error(s);
            break;
        }
        embi_js_key k = list->data[i];
        uint32_t mark = s->out.len;
        if (any) {
            put(s, ',');
        }
        if (s->gap_len > 0) {
            newline_indent(s, level);
        }
        quote(s, embi_js_str_flat(rt, embi_js_key_to_value(rt, k)));
        put(s, ':');
        if (s->gap_len > 0) {
            put(s, ' ');
        }
        int r = ser_property(s, k, o);
        if (r == EMBI_JS_THREW) {
            result = EMBI_JS_THREW;
            break;
        }
        if (r == EMBI_JS_NO) {
            s->out.len = mark; /* undefined: the member is omitted, separator and all */
        } else {
            any = true;
        }
    }
    if (result == EMBI_JS_YES) {
        if (any && s->gap_len > 0) {
            newline_indent(s, level - 1);
        }
        put(s, '}');
    }
    embi_vec_free(&own);
    s->stack.len--;
    return result;
}

/* SerializeJSONArray. Undefined elements become null. */
static int ser_array(ser *s, embi_js_object *a)
{
    embi_js_runtime *rt = s->rt;
    if (!enter(s, a)) {
        return EMBI_JS_THREW;
    }
    double len;
    if (!embi_js_length_of_array_like(rt, a, &len)) {
        s->stack.len--;
        return EMBI_JS_THREW;
    }
    uint32_t level = s->stack.len;
    int result = EMBI_JS_YES;
    put(s, '[');
    for (double i = 0.0; i < len; i++) {
        if (s->overflow) {
            result = overflow_error(s);
            break;
        }
        if (i > 0.0) {
            put(s, ',');
        }
        if (s->gap_len > 0) {
            newline_indent(s, level);
        }
        embi_js_key k = index_key(rt, i);
        if (k == EXC) {
            result = EMBI_JS_THREW;
            break;
        }
        int r = ser_property(s, k, a);
        if (r == EMBI_JS_THREW) {
            result = EMBI_JS_THREW;
            break;
        }
        if (r == EMBI_JS_NO) {
            put_ascii(s, "null");
        }
    }
    if (result == EMBI_JS_YES) {
        if (len > 0.0 && s->gap_len > 0) {
            newline_indent(s, level - 1);
        }
        put(s, ']');
    }
    s->stack.len--;
    return result;
}

/* SerializeJSONProperty(key, holder). Returns YES when a value was written, NO when the value is
 * undefined (nothing written), or THREW with the exception pending. */
static int ser_property(ser *s, embi_js_key key, embi_js_object *holder)
{
    embi_js_runtime *rt = s->rt;
    embi_js_value v = embi_js_obj_get(rt, holder, key, embi_js_from_object(holder));
    if (v == EXC) {
        return EMBI_JS_THREW;
    }
    if (embi_js_is_object(v) || embi_js_is_bigint(v)) {
        embi_js_value tj = embi_js_get_v(rt, v, s->tojson_key);
        if (tj == EXC) {
            return EMBI_JS_THREW;
        }
        if (embi_js_is_callable(tj)) {
            embi_js_value kv = embi_js_key_to_value(rt, key);
            v = embi_js_call(rt, tj, v, 1, &kv);
            if (v == EXC) {
                return EMBI_JS_THREW;
            }
        }
    }
    if (!embi_js_is_undefined(s->fn)) {
        embi_js_value args[2] = {embi_js_key_to_value(rt, key), v};
        v = embi_js_call(rt, s->fn, embi_js_from_object(holder), 2, args);
        if (v == EXC) {
            return EMBI_JS_THREW;
        }
    }
    if (embi_js_is_object(v)) {
        embi_js_object *o = embi_js_as_object(v);
        const embi_js_class *k = o->klass;
        if (k == &embi_js_class_number) {
            double d;
            if (!embi_js_to_number(rt, v, &d)) {
                return EMBI_JS_THREW;
            }
            v = embi_js_number(d);
        } else if (k == &embi_js_class_string) {
            embi_js_string *str = embi_js_to_string(rt, v);
            if (!str) {
                return EMBI_JS_THREW;
            }
            v = embi_js_from_string(str);
        } else if (k == &embi_js_class_boolean) {
            v = embi_js_wrapper_value(o);
        } else if (k == &embi_js_class_bigint) {
            v = embi_js_bigint_wrapper_value(o);
        }
    }
    if (embi_js_is_object(v) && embi_js_as_object(v)->klass == &class_raw_json) {
        /* A raw JSON object serializes as its own text (ES2026). */
        embi_js_value raw = embi_js_obj_get(rt, embi_js_as_object(v), embi_js_key_c(rt, "rawJSON"), v);
        if (raw == EXC) {
            return EMBI_JS_THREW;
        }
        const embi_js_string *rs = embi_js_str_flat(rt, raw);
        for (uint32_t i = 0; i < embi_js_str_len(rs); i++) {
            put(s, embi_js_str_at(rs, i));
        }
        return EMBI_JS_YES;
    }
    if (v == EMBI_JS_NULL) {
        put_ascii(s, "null");
        return EMBI_JS_YES;
    }
    if (v == EMBI_JS_TRUE) {
        put_ascii(s, "true");
        return EMBI_JS_YES;
    }
    if (v == EMBI_JS_FALSE) {
        put_ascii(s, "false");
        return EMBI_JS_YES;
    }
    if (embi_js_is_string(v)) {
        quote(s, embi_js_str_flat(s->rt, v));
        return EMBI_JS_YES;
    }
    if (embi_js_is_bigint(v)) {
        (void)embi_js_throw_type_error(rt, "Do not know how to serialize a BigInt");
        return EMBI_JS_THREW;
    }
    if (embi_js_is_number(v)) {
        if (embi_js_is_int32(v)) {
            put_number(s, v);
        } else {
            double d = embi_js_as_double(v);
            if (d - d == 0.0) { /* finite */
                put_number(s, v);
            } else {
                put_ascii(s, "null");
            }
        }
        return EMBI_JS_YES;
    }
    if (embi_js_is_object(v) && !embi_js_is_callable(v)) {
        embi_js_object *o = embi_js_as_object(v);
        int v_is_arr = embi_js_is_array_checked(rt, v);
        if (v_is_arr == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        return v_is_arr == EMBI_JS_YES ? ser_array(s, o) : ser_object(s, o);
    }
    return EMBI_JS_NO; /* undefined, symbols and functions */
}

/* PropertyList from an array replacer: string, number and wrapper items, first occurrence kept. */
static bool build_props(ser *s, embi_js_object *ra)
{
    embi_js_runtime *rt = s->rt;
    double len;
    if (!embi_js_length_of_array_like(rt, ra, &len)) {
        return false;
    }
    for (double i = 0.0; i < len; i++) {
        embi_js_key ik = index_key(rt, i);
        if (ik == EXC) {
            return false;
        }
        embi_js_value v = embi_js_obj_get(rt, ra, ik, embi_js_from_object(ra));
        if (v == EXC) {
            return false;
        }
        embi_js_string *item = nullptr;
        if (embi_js_is_string(v)) {
            item = embi_js_str_flat(rt, v);
        } else if (embi_js_is_number(v)) {
            item = embi_js_to_string(rt, v);
            if (!item) {
                return false;
            }
        } else if (embi_js_is_object(v)) {
            const embi_js_class *k = embi_js_as_object(v)->klass;
            if (k == &embi_js_class_string || k == &embi_js_class_number) {
                item = embi_js_to_string(rt, v);
                if (!item) {
                    return false;
                }
            }
        }
        if (!item) {
            continue;
        }
        embi_js_key pk = embi_js_key_from_str(rt, item);
        bool dup = false;
        for (uint32_t j = 0; j < s->props.len && !dup; j++) {
            dup = s->props.data[j] == pk;
        }
        if (!dup) {
            embi_vec_push(&s->props, pk);
        }
    }
    s->has_props = true;
    return true;
}

/* The gap from the space argument: up to 10 spaces, or the first 10 units of a string. */
static bool read_gap(ser *s, embi_js_value space)
{
    embi_js_runtime *rt = s->rt;
    if (embi_js_is_object(space)) {
        const embi_js_class *k = embi_js_as_object(space)->klass;
        if (k == &embi_js_class_number) {
            double d;
            if (!embi_js_to_number(rt, space, &d)) {
                return false;
            }
            space = embi_js_number(d);
        } else if (k == &embi_js_class_string) {
            embi_js_string *str = embi_js_to_string(rt, space);
            if (!str) {
                return false;
            }
            space = embi_js_from_string(str);
        }
    }
    if (embi_js_is_number(space)) {
        double n;
        if (!embi_js_to_integer_or_infinity(rt, space, &n)) {
            return false;
        }
        if (n >= 1.0) {
            uint32_t count = n > (double)JSON_GAP_MAX ? JSON_GAP_MAX : (uint32_t)n;
            for (uint32_t i = 0; i < count; i++) {
                s->gap[s->gap_len++] = ' ';
            }
        }
    } else if (embi_js_is_string(space)) {
        const embi_js_string *str = embi_js_str_flat(s->rt, space);
        uint32_t n = embi_js_str_len(str);
        if (n > JSON_GAP_MAX) {
            n = JSON_GAP_MAX;
        }
        for (uint32_t i = 0; i < n; i++) {
            s->gap[s->gap_len++] = embi_js_str_at(str, i);
        }
    }
    return true;
}

static embi_js_value json_stringify(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value value = argc > 0 ? argv[0] : UNDEF;
    embi_js_value replacer = argc > 1 ? argv[1] : UNDEF;
    embi_js_value space = argc > 2 ? argv[2] : UNDEF;

    ser s;
    memset(&s, 0, sizeof s);
    s.rt = rt;
    s.fn = UNDEF;
    s.tojson_key = embi_js_key_c(rt, "toJSON");

    embi_js_value result = EXC;
    embi_js_object *wrapper = nullptr;
    embi_js_key empty = 0;
    int r2 = EMBI_JS_THREW;

    if (embi_js_is_object(replacer)) {
        if (embi_js_is_callable(replacer)) {
            s.fn = replacer;
        } else {
            int rep_is_arr = embi_js_is_array_checked(rt, replacer);
            if (rep_is_arr == EMBI_JS_THREW) {
                goto done;
            }
            if (rep_is_arr == EMBI_JS_YES && !build_props(&s, embi_js_as_object(replacer))) {
                goto done;
            }
        }
    }
    if (!read_gap(&s, space)) {
        goto done;
    }
    wrapper = embi_js_object_new_plain(rt);
    empty = embi_js_key_c(rt, "");
    if (embi_js_create_data_property_or_throw(rt, wrapper, empty, value) != EMBI_JS_YES) {
        goto done;
    }
    r2 = ser_property(&s, empty, wrapper);
    if (r2 == EMBI_JS_THREW) {
        goto done;
    }
    if (s.overflow) {
        (void)overflow_error(&s);
        goto done;
    }
    if (r2 == EMBI_JS_NO) {
        result = UNDEF;
    } else {
        embi_js_string *out = embi_js_str_new_utf16(rt, s.out.data, s.out.len);
        result = embi_js_from_string(out);
    }

done:
    ser_free(&s);
    return result;
}

void embi_js_init_builtins_json(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *j = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_define_method(r, j, "parse", json_parse, 2, 0);
    embi_js_define_method(r, j, "stringify", json_stringify, 3, 0);
    embi_js_define_method(r, j, "rawJSON", json_raw_json, 1, 0);
    embi_js_define_method(r, j, "isRawJSON", json_is_raw_json, 1, 0);
    embi_js_value tag = embi_js_from_string(embi_js_atom_intern_utf8(rt, "JSON", 4));
    embi_js_define_raw(rt, j, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), tag, ATTR_C);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "JSON"), embi_js_from_object(j),
                       ATTR_WC);
}
