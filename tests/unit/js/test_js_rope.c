#include "emb_test.h"

#include "js/js.h"

/* J5f rope core: construction, flattening, the rope-safe queries,
 * atoms and keys, the GC trace and its barrier, and the depth bound. Producers come in J5g, so
 * these tests build ropes directly through embi_js_str_concat_v with rope_min set to 1 (or the
 * default 256). */

static embi_js_value V(const embi_js_string *s)
{
    return embi_js_from_string(s);
}

static embi_js_string *latin(embi_js_runtime *rt, const char *s)
{
    return embi_js_str_new_utf8(rt, s, strlen(s));
}

static embi_js_value cat(embi_js_runtime *rt, embi_js_value a, embi_js_value b)
{
    return embi_js_str_concat_v(rt, a, b);
}

static bool is_rope_v(embi_js_value v)
{
    return embi_js_str_is_rope(embi_js_as_string(v));
}

static bool is_marked_v(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->mark != 0;
}

/* Content check through the flat form. */
static bool content_is(embi_js_runtime *rt, embi_js_value v, const char *ascii)
{
    embi_js_string *f = embi_js_str_flat(rt, v);
    return embi_js_str_equal(f, latin(rt, ascii));
}

/* A rope of two flat halves. */
static embi_js_value make_pair_rope(embi_js_runtime *rt, const char *x, const char *y)
{
    return cat(rt, V(latin(rt, x)), V(latin(rt, y)));
}

/* A flat Latin-1 string of len copies of ch (len at most 512). */
static embi_js_value fill_latin(embi_js_runtime *rt, uint8_t ch, uint32_t len)
{
    uint8_t buf[512];
    EMB_CHECK(len <= sizeof buf);
    memset(buf, ch, len);
    return V(embi_js_str_new_latin1(rt, buf, len));
}

/* Stack-scrubbing helper, as in test_js_gc_gen.c: clears the stale frames below the caller so the
 * conservative scan does not keep an unreachable cell alive. */
static EMB_TEST_NOIPA void scrub_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub() scrub_impl(0, 0, 0, 0)

EMB_TEST(rope_build_flatten_and_indirection)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_value r = make_pair_rope(rt, "hello, ", "world");
    EMB_CHECK(is_rope_v(r));
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(r)), 12);
    EMB_CHECK(!embi_js_str_is_wide(embi_js_as_string(r)));
    EMB_CHECK(embi_js_str_equal(embi_js_as_string(r), latin(rt, "hello, world")));

    embi_js_string *f1 = embi_js_str_flat(rt, r);
    EMB_CHECK(!embi_js_str_is_rope(f1));
    EMB_CHECK(content_is(rt, V(f1), "hello, world"));
    embi_js_string *f2 = embi_js_str_flat(rt, r);
    EMB_CHECK(f1 == f2);

    /* The rope is now an indirection: left is the flat string, right is EMPTY. */
    const embi_js_rope *node = (const embi_js_rope *)embi_js_as_string(r);
    EMB_CHECK((node->hdr.flags & EMBI_JS_STR_FLATTENED) != 0);
    EMB_CHECK(node->left == V(f1));
    EMB_CHECK(node->right == EMBI_JS_EMPTY);

    /* Concatenating an indirection uses its flat string. */
    embi_js_value r2 = cat(rt, r, V(latin(rt, "!")));
    EMB_CHECK(content_is(rt, r2, "hello, world!"));
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_empty_operands_and_threshold)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK_EQ_INT(rt->rope_min, 256);
    embi_js_value e = V(latin(rt, ""));
    embi_js_value b = V(latin(rt, "abc"));
    /* An empty side returns the other side unchanged. */
    EMB_CHECK(cat(rt, e, b) == b);
    EMB_CHECK(cat(rt, b, e) == b);

    /* Below the default threshold (256 units) the result is flat; at or above it, a rope. */
    uint8_t buf127[127];
    uint8_t buf128[128];
    memset(buf127, 'a', sizeof buf127);
    memset(buf128, 'b', sizeof buf128);
    embi_js_value a127 = V(embi_js_str_new_latin1(rt, buf127, sizeof buf127));
    embi_js_value a128 = V(embi_js_str_new_latin1(rt, buf128, sizeof buf128));
    embi_js_value b127 = V(embi_js_str_new_latin1(rt, buf128, sizeof buf127));
    embi_js_value b128 = V(embi_js_str_new_latin1(rt, buf128, sizeof buf128));
    EMB_CHECK(!is_rope_v(cat(rt, a127, b127))); /* 254 units: flat */
    EMB_CHECK(!is_rope_v(cat(rt, a127, b128))); /* 255 units: flat */
    EMB_CHECK(is_rope_v(cat(rt, a128, b128)));  /* 256 units: rope */
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(cat(rt, a128, b128))), 256);
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_equality_hash_and_compare_match_flat)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_value r = cat(rt, make_pair_rope(rt, "ab", "cd"), V(latin(rt, "ef")));
    EMB_CHECK(is_rope_v(r));
    const embi_js_string *rs = embi_js_as_string(r);
    embi_js_string *flat = latin(rt, "abcdef");
    EMB_CHECK(embi_js_str_equal(rs, flat));
    EMB_CHECK(embi_js_str_equal(flat, rs));
    EMB_CHECK_EQ_INT(embi_js_str_compare(rs, flat), 0);
    EMB_CHECK_EQ_INT(embi_js_str_hash(rs), embi_js_str_hash(flat));

    embi_js_value other = make_pair_rope(rt, "abcde", "g");
    EMB_CHECK(!embi_js_str_equal(embi_js_as_string(other), flat));
    EMB_CHECK(embi_js_str_compare(embi_js_as_string(other), flat) > 0);
    EMB_CHECK(embi_js_str_compare(flat, embi_js_as_string(other)) < 0);

    /* A proper prefix compares less. */
    embi_js_value pre = make_pair_rope(rt, "abc", "de");
    EMB_CHECK(embi_js_str_compare(embi_js_as_string(pre), flat) < 0);
    EMB_CHECK(embi_js_str_compare(flat, embi_js_as_string(pre)) > 0);
    EMB_CHECK(!embi_js_str_equal(embi_js_as_string(pre), flat));

    /* The hash and the content survive flattening. */
    uint32_t h_before = embi_js_str_hash(rs);
    (void)embi_js_str_flat(rt, r);
    EMB_CHECK_EQ_INT(embi_js_str_hash(embi_js_as_string(r)), h_before);
    EMB_CHECK(embi_js_str_equal(embi_js_as_string(r), flat));
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_wide_mixing_and_surrogates)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    /* Narrow + wide makes a wide rope; the flat form has the same units. */
    embi_js_string *w = embi_js_str_new_utf16(rt, (const uint16_t[]){0x3B1}, 1);
    embi_js_value r = cat(rt, V(latin(rt, "ab")), V(w));
    EMB_CHECK(is_rope_v(r));
    EMB_CHECK(embi_js_str_is_wide(embi_js_as_string(r)));
    embi_js_string *f = embi_js_str_flat(rt, r);
    EMB_CHECK(embi_js_str_is_wide(f));
    EMB_CHECK_EQ_INT(embi_js_str_len(f), 3);
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 0), 'a');
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 1), 'b');
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 2), 0x3B1);

    /* A surrogate pair split across two leaves is one code point in UTF-8. */
    embi_js_string *hi = embi_js_str_new_utf16(rt, (const uint16_t[]){0xD83D}, 1);
    embi_js_string *lo = embi_js_str_new_utf16(rt, (const uint16_t[]){0xDE00}, 1);
    embi_js_value pair = cat(rt, V(hi), V(lo));
    EMB_CHECK(is_rope_v(pair));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(pair), &sb);
    EMB_CHECK(strcmp(embi_sb_cstr(&sb), "\xF0\x9F\x98\x80") == 0);
    embi_sb_free(&sb);
    embi_js_runtime_free(rt);
}

/* A rope of a wide and a narrow leaf is not equal to a narrow rope of the same length, and it
 * compares by code unit (0x3B1 is above every narrow unit). */
EMB_TEST(rope_wide_and_narrow_ropes_of_equal_length_differ)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_string *w = embi_js_str_new_utf16(rt, (const uint16_t[]){0x3B1}, 1);
    embi_js_value wide_rope = cat(rt, fill_latin(rt, 'a', 300), V(w));
    embi_js_value narrow_rope = cat(rt, fill_latin(rt, 'a', 300), fill_latin(rt, 'a', 1));
    EMB_CHECK(is_rope_v(wide_rope));
    EMB_CHECK(is_rope_v(narrow_rope));
    EMB_CHECK(embi_js_str_is_wide(embi_js_as_string(wide_rope)));
    EMB_CHECK(!embi_js_str_is_wide(embi_js_as_string(narrow_rope)));
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(wide_rope)), 301);
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(narrow_rope)), 301);
    EMB_CHECK(!embi_js_str_equal(embi_js_as_string(wide_rope), embi_js_as_string(narrow_rope)));
    EMB_CHECK(embi_js_str_compare(embi_js_as_string(wide_rope), embi_js_as_string(narrow_rope)) > 0);
    EMB_CHECK(embi_js_str_compare(embi_js_as_string(narrow_rope), embi_js_as_string(wide_rope)) < 0);

    /* The wide rope equals the wide flat string with the same units, hash included. */
    uint16_t units[301];
    for (uint32_t i = 0; i < 300; i++) {
        units[i] = 'a';
    }
    units[300] = 0x3B1;
    embi_js_string *expected = embi_js_str_new_utf16(rt, units, 301);
    EMB_CHECK(embi_js_str_is_wide(expected));
    EMB_CHECK(embi_js_str_equal(embi_js_as_string(wide_rope), expected));
    EMB_CHECK_EQ_INT(embi_js_str_compare(embi_js_as_string(wide_rope), expected), 0);
    EMB_CHECK_EQ_INT(embi_js_str_hash(embi_js_as_string(wide_rope)), embi_js_str_hash(expected));
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_long_append_chain_stays_shallow_and_correct)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_value acc = V(latin(rt, "x"));
    const int count = 5000;
    for (int i = 0; i < count; i++) {
        uint8_t c = (uint8_t)('a' + i % 26);
        embi_js_value piece = V(embi_js_str_new_latin1(rt, &c, 1));
        acc = cat(rt, acc, piece);
        if (i % 700 == 0) {
            embi_js_gc(rt); /* the rope and its leaves survive collections mid-chain */
        }
    }
    const embi_js_string *rs = embi_js_as_string(acc);
    EMB_CHECK_EQ_INT(embi_js_str_len(rs), (uint32_t)count + 1u);
    /* Single-character appends merge into short leaves, so the depth stays small. */
    EMB_CHECK(rs->hdr.aux >= 1u);
    EMB_CHECK(rs->hdr.aux <= 64u);

    embi_js_string *f = embi_js_str_flat(rt, acc);
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 0), 'x');
    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (embi_js_str_at(f, (uint32_t)i + 1u) != (uint16_t)('a' + i % 26)) {
            ok = false;
            break;
        }
    }
    EMB_CHECK(ok);
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_depth_is_bounded_by_flattening)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    /* 300-unit pieces never merge, so every concat adds one level until the bound applies. */
    const uint32_t piece_len = 300u;
    const int pieces = 4200;
    uint8_t buf[300];
    memset(buf, 'a', sizeof buf);
    embi_js_value acc = V(embi_js_str_new_latin1(rt, buf, piece_len));
    uint32_t max_depth = 0;
    for (int i = 1; i < pieces; i++) {
        memset(buf, 'a' + i % 26, sizeof buf);
        embi_js_value piece = V(embi_js_str_new_latin1(rt, buf, piece_len));
        acc = cat(rt, acc, piece);
        uint32_t d = embi_js_as_string(acc)->hdr.aux;
        if (d > max_depth) {
            max_depth = d;
        }
    }
    EMB_CHECK(max_depth > 0u);
    EMB_CHECK(max_depth <= EMBI_JS_ROPE_DEPTH_MAX + 1u);
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(acc)), (uint32_t)pieces * piece_len);

    embi_js_string *f = embi_js_str_flat(rt, acc);
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 0), 'a');
    EMB_CHECK_EQ_INT(embi_js_str_at(f, piece_len), (uint16_t)('a' + 1));
    EMB_CHECK_EQ_INT(embi_js_str_at(f, (uint32_t)(pieces - 1) * piece_len),
                     (uint16_t)('a' + (pieces - 1) % 26));
    embi_js_runtime_free(rt);
}

/* ------------------------------------------------------------------ deep shapes */

#define DEEP_PIECE 300u

/* Piece i: DEEP_PIECE copies of 'a' + i % 26, or of '#' when i == skew. */
static embi_js_value deep_piece(embi_js_runtime *rt, int i, int skew)
{
    uint8_t ch = i == skew ? (uint8_t)'#' : (uint8_t)('a' + i % 26);
    return fill_latin(rt, ch, DEEP_PIECE);
}

/* Pieces [from, to) as a left-leaning chain: ((p0 + p1) + p2) + ... */
static embi_js_value build_left(embi_js_runtime *rt, int from, int to, int skew)
{
    embi_js_value acc = deep_piece(rt, from, skew);
    for (int i = from + 1; i < to; i++) {
        acc = cat(rt, acc, deep_piece(rt, i, skew));
    }
    return acc;
}

/* Pieces [from, to) as a right-leaning chain: p0 + (p1 + (p2 + ...)). */
static embi_js_value build_right(embi_js_runtime *rt, int from, int to, int skew)
{
    embi_js_value acc = deep_piece(rt, to - 1, skew);
    for (int i = to - 2; i >= from; i--) {
        acc = cat(rt, deep_piece(rt, i, skew), acc);
    }
    return acc;
}

/* The same 48 pieces, split at different points and shaped differently: the left-leaning and
 * right-leaning chains, and a left half joined to a right half. Each non-merging piece is a leaf,
 * so the walkers see more than 40 leaves, and the left-leaning chain pushes past the initial walker
 * stack capacity of 16. */
EMB_TEST(rope_deep_shapes_equal_compare_and_hash_alike)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    const int n = 48;
    embi_js_value a_v = build_left(rt, 0, n, -1);
    embi_js_value b_v = build_right(rt, 0, n, -1);
    embi_js_value left_half = build_left(rt, 0, n / 2, -1);
    embi_js_value right_half = build_right(rt, n / 2, n, -1);
    embi_js_value c_v = cat(rt, left_half, right_half);
    EMB_CHECK(is_rope_v(a_v));
    EMB_CHECK(is_rope_v(b_v));
    EMB_CHECK(is_rope_v(c_v));
    EMB_CHECK(embi_js_as_string(a_v)->hdr.aux > 40u);
    EMB_CHECK(embi_js_as_string(b_v)->hdr.aux > 40u);
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(a_v)), (uint32_t)n * DEEP_PIECE);

    const embi_js_string *a = embi_js_as_string(a_v);
    const embi_js_string *b = embi_js_as_string(b_v);
    const embi_js_string *c = embi_js_as_string(c_v);
    EMB_CHECK(embi_js_str_equal(a, b));
    EMB_CHECK(embi_js_str_equal(b, a));
    EMB_CHECK(embi_js_str_equal(a, c));
    EMB_CHECK(embi_js_str_equal(c, b));
    EMB_CHECK_EQ_INT(embi_js_str_compare(a, b), 0);
    EMB_CHECK_EQ_INT(embi_js_str_compare(b, c), 0);
    uint32_t h = embi_js_str_hash(a);
    EMB_CHECK_EQ_INT(embi_js_str_hash(b), h);
    EMB_CHECK_EQ_INT(embi_js_str_hash(c), h);

    /* A flat copy of the same content agrees with every shape. */
    embi_js_string *f = embi_js_str_flat(rt, a_v);
    EMB_CHECK(embi_js_str_equal(b, f));
    EMB_CHECK(embi_js_str_equal(c, f));
    EMB_CHECK_EQ_INT(embi_js_str_compare(c, f), 0);
    EMB_CHECK_EQ_INT(embi_js_str_hash(f), h);

    /* One piece differs: unequal, and ordered by the first differing unit. 'a' + 17 is above '#'. */
    embi_js_value d_v = build_right(rt, 0, n, 17);
    const embi_js_string *d = embi_js_as_string(d_v);
    EMB_CHECK(!embi_js_str_equal(a, d));
    EMB_CHECK(!embi_js_str_equal(d, a));
    EMB_CHECK(embi_js_str_compare(a, d) > 0);
    EMB_CHECK(embi_js_str_compare(d, a) < 0);
    embi_js_runtime_free(rt);
}

/* A rope whose child is a rope that was already flattened. The child stays a node (its
 * indirection), and the walkers, the queries and the flat form must follow it. */
EMB_TEST(rope_child_already_flattened_is_rope_safe)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_value x_v = cat(rt, fill_latin(rt, 'x', 300), fill_latin(rt, 'y', 300));
    EMB_CHECK(is_rope_v(x_v));
    embi_js_value r_v = cat(rt, x_v, fill_latin(rt, 'z', 300));
    EMB_CHECK(is_rope_v(r_v));
    /* X is a live rope when R is built, so R's left child is X's node itself. */
    EMB_CHECK(((const embi_js_rope *)embi_js_as_string(r_v))->left == x_v);

    (void)embi_js_str_flat(rt, x_v); /* X becomes an indirection; R's child is now flattened */
    EMB_CHECK((embi_js_as_string(x_v)->hdr.flags & EMBI_JS_STR_FLATTENED) != 0);

    uint8_t expected_units[900];
    memset(expected_units, 'x', 300);
    memset(expected_units + 300, 'y', 300);
    memset(expected_units + 600, 'z', 300);
    embi_js_string *expected = embi_js_str_new_latin1(rt, expected_units, sizeof expected_units);
    const embi_js_string *r = embi_js_as_string(r_v);
    EMB_CHECK(embi_js_str_equal(r, expected));
    EMB_CHECK(embi_js_str_equal(expected, r));
    EMB_CHECK_EQ_INT(embi_js_str_compare(r, expected), 0);
    EMB_CHECK_EQ_INT(embi_js_str_hash(r), embi_js_str_hash(expected));

    embi_js_string *f = embi_js_str_flat(rt, r_v);
    EMB_CHECK(embi_js_str_equal(f, expected));
    EMB_CHECK_EQ_INT(embi_js_str_hash(f), embi_js_str_hash(expected));
    embi_js_runtime_free(rt);
}

/* A rope with an empty leaf. The public constructors never build one (an empty operand returns the
 * other side), so this node is made by hand. The walkers must skip the empty leaf. */
EMB_TEST(rope_empty_leaf_is_skipped_by_walks)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    /* A rope first, so the STRING cell type has its trace function. */
    (void)cat(rt, fill_latin(rt, 'a', 300), fill_latin(rt, 'b', 300));

    embi_js_value left = fill_latin(rt, 'q', 300);
    embi_js_string *empty = embi_js_str_new_latin1(rt, nullptr, 0);
    embi_js_rope *node = (embi_js_rope *)embi_js_alloc_cell(rt, sizeof *node, EMBI_JS_CELL_STRING);
    node->len_flags = embi_js_str_len(embi_js_as_string(left));
    node->hdr.flags = EMBI_JS_STR_ROPE;
    node->hdr.aux = 1;
    node->hash = 0;
    node->left = left;
    node->right = V(empty);
    embi_js_value r_v = embi_js_from_cell(node);

    EMB_CHECK(is_rope_v(r_v));
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(r_v)), 300);
    embi_js_value expected_v = fill_latin(rt, 'q', 300);
    const embi_js_string *expected = embi_js_as_string(expected_v);
    EMB_CHECK(embi_js_str_equal(embi_js_as_string(r_v), expected));
    EMB_CHECK_EQ_INT(embi_js_str_compare(embi_js_as_string(r_v), expected), 0);
    EMB_CHECK_EQ_INT(embi_js_str_hash(embi_js_as_string(r_v)), embi_js_str_hash(expected));

    embi_js_string *f = embi_js_str_flat(rt, r_v);
    EMB_CHECK(embi_js_str_equal(f, expected));
    embi_js_runtime_free(rt);
}

/* ------------------------------------------------------------------ atoms and char consumers */

EMB_TEST(rope_atoms_and_keys_are_flat)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_string *flat_atom = embi_js_atom_intern(rt, latin(rt, "abcd"));
    embi_js_value r = make_pair_rope(rt, "ab", "cd");
    embi_js_string *atom = embi_js_atom_intern(rt, embi_js_as_string(r));
    EMB_CHECK(atom == flat_atom);
    EMB_CHECK(embi_js_str_is_atom(atom));
    EMB_CHECK(!embi_js_str_is_rope(atom));

    /* A rope that spells an array index is an index key. */
    embi_js_value digits = make_pair_rope(rt, "12", "3");
    embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(digits));
    EMB_CHECK(embi_js_key_is_index(k));
    EMB_CHECK_EQ_INT(embi_js_key_index(k), 123);

    /* A rope that spells a name is the atom for that name. */
    embi_js_value name = make_pair_rope(rt, "ab", "cd");
    embi_js_key kn = embi_js_key_from_str(rt, embi_js_as_string(name));
    EMB_CHECK(!embi_js_key_is_index(kn));
    EMB_CHECK(embi_js_as_string(kn) == atom);
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_char_consumers_accept_ropes)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    /* Number parsing with whitespace on both sides. */
    embi_js_value num = make_pair_rope(rt, " 12", ".5 ");
    EMB_CHECK(embi_js_str_to_number(embi_js_as_string(num)) == 12.5);

    /* Array index check on a rope. */
    uint32_t idx = 0;
    EMB_CHECK(embi_js_str_is_array_index(embi_js_as_string(make_pair_rope(rt, "4", "2")), &idx));
    EMB_CHECK_EQ_INT(idx, 42);

    /* UTF-8 append of a narrow rope with a non-ASCII unit. */
    uint8_t e9 = 0xE9;
    embi_js_value na = cat(rt, V(latin(rt, "a")), V(embi_js_str_new_latin1(rt, &e9, 1)));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(na), &sb);
    EMB_CHECK(strcmp(embi_sb_cstr(&sb), "a\xC3\xA9") == 0);
    embi_sb_free(&sb);

    /* substring and the flat concatenation accept ropes. */
    embi_js_value r = make_pair_rope(rt, "abc", "def");
    embi_js_string *sub = embi_js_str_substring(rt, embi_js_as_string(r), 2, 4);
    EMB_CHECK(content_is(rt, V(sub), "cd"));
    embi_js_string *fc = embi_js_str_concat(rt, embi_js_as_string(r), latin(rt, "!"));
    EMB_CHECK(!embi_js_str_is_rope(fc));
    EMB_CHECK(content_is(rt, V(fc), "abcdef!"));
    embi_js_runtime_free(rt);
}

/* ------------------------------------------------------------------ GC trace and barrier */

/* Builds a rope of two leaves and keeps only the rope alive (a persistent handle). Weak handles
 * observe the leaves without keeping them alive. Out of line, so the frame that made the leaves is
 * gone before any collection runs. */
static EMB_TEST_NOIPA void build_pair_with_weak_leaves(embi_js_runtime *rt, uint32_t *rope_id,
                                                       uint32_t *weak_left, uint32_t *weak_right)
{
    embi_js_value l = V(latin(rt, "left-part-"));
    embi_js_value r = V(latin(rt, "right-part"));
    *weak_left = embi_js_weak_new(rt, l);
    *weak_right = embi_js_weak_new(rt, r);
    *rope_id = embi_js_persistent_new(rt, cat(rt, l, r));
}

static EMB_TEST_NOIPA void flatten_persistent(embi_js_runtime *rt, uint32_t id)
{
    (void)embi_js_str_flat(rt, embi_js_persistent_get(rt, id));
}

/* The leaves are reachable only through the rope. Before the rope is flattened, the STRING trace
 * function keeps them alive. After flattening they are unreachable and die, which is the point of
 * flattening. Weak handles make both claims without the conservative stack getting in the way. */
EMB_TEST(rope_children_are_traced_until_flatten_then_die)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    uint32_t id = 0;
    uint32_t wl = 0;
    uint32_t wr = 0;
    build_pair_with_weak_leaves(rt, &id, &wl, &wr);
    scrub();
    embi_js_gc(rt);

    EMB_CHECK(embi_js_weak_get(rt, wl) != EMBI_JS_EMPTY);
    EMB_CHECK(embi_js_weak_get(rt, wr) != EMBI_JS_EMPTY);
    /* The content reads through the leaves without flattening. */
    EMB_CHECK(embi_js_str_equal(embi_js_as_string(embi_js_persistent_get(rt, id)),
                                latin(rt, "left-part-right-part")));

    flatten_persistent(rt, id);
    scrub();
    embi_js_gc(rt);
    /* Under AddressSanitizer a stale stack word can still name a leaf, so only the live side is
     * exact there (see EMB_TEST_ASAN in emb_test.h). */
    EMB_CHECK(EMB_TEST_ASAN || embi_js_weak_get(rt, wl) == EMBI_JS_EMPTY);
    EMB_CHECK(EMB_TEST_ASAN || embi_js_weak_get(rt, wr) == EMBI_JS_EMPTY);
    EMB_CHECK(content_is(rt, embi_js_persistent_get(rt, id), "left-part-right-part"));

    embi_js_persistent_free(rt, id);
    embi_js_weak_free(rt, wl);
    embi_js_weak_free(rt, wr);
    embi_js_runtime_free(rt);
}

static EMB_TEST_NOIPA void gen_pair_persistent(embi_js_runtime *rt, uint32_t *id)
{
    *id = embi_js_persistent_new(rt, make_pair_rope(rt, "gen-left-", "gen-right"));
}

/* The rope becomes old, and then a young flat string is stored into it by flattening. Only the
 * write barrier in flatten_rope tells the minor collection about the new child. The flat string is
 * reachable from nothing else. Under debug builds the minor also verifies the remembered set. */
static EMB_TEST_NOIPA void old_rope_flatten_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_gc_set_generational(rt, true);
    uint32_t id = 0;
    gen_pair_persistent(rt, &id);
    embi_js_gc(rt); /* a major: the rope is old now (sticky mark) */
    scrub();
    EMB_CHECK(is_marked_v(embi_js_persistent_get(rt, id)));

    flatten_persistent(rt, id); /* young flat stored into an old rope: needs the barrier */
    scrub();

    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    embi_js_gc_minor(rt);
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    EMB_CHECK(s1.minor_count == s0.minor_count + 1u);
    EMB_CHECK(s1.major_count == s0.major_count);

    /* Reuse freed memory: a freed flat string would be overwritten here. */
    for (int i = 0; i < 2000; i++) {
        (void)latin(rt, "ZZZZZZZZZZZZZZZZ");
    }
    scrub();
    embi_js_value v = embi_js_persistent_get(rt, id);
    EMB_CHECK(content_is(rt, v, "gen-left-gen-right"));
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(rope_flatten_of_old_rope_keeps_young_flat_through_minor)
{
    scrub();
    old_rope_flatten_body();
    scrub();
}

/* ------------------------------------------------------------------ lifetime */

EMB_TEST(rope_cells_are_freed_with_the_runtime)
{
    size_t base = embi_mem_live();
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    embi_js_value acc = V(latin(rt, "seed"));
    for (int i = 0; i < 2000; i++) {
        acc = cat(rt, acc, V(latin(rt, "xy")));
    }
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_as_string(acc)), 4u + 4000u);
    embi_js_string *f = embi_js_str_flat(rt, acc);
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 0), 's');
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 4), 'x');
    EMB_CHECK_EQ_INT(embi_js_str_at(f, 4003), 'y');
    embi_js_runtime_free(rt);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(rope_min_default_restored_by_zero)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_rope_min(rt, 1);
    EMB_CHECK_EQ_INT(rt->rope_min, 1);
    embi_js_set_rope_min(rt, 0);
    EMB_CHECK_EQ_INT(rt->rope_min, 256);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
