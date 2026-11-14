/*
 * Encoding Standard tables: every single-byte encoding decodes its bytes exactly as the vendored
 * WHATWG index file says, labels resolve as the standard lists them, and the unsupported CJK
 * labels are recognized (so callers can report them) rather than treated as unknown.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

#include <SDL3/SDL_iostream.h>

/* The single-byte encodings with an index file under tools/unicodegen/data/encoding. */
static const char *const single_byte[] = {
    "ibm866",        "iso-8859-2",   "iso-8859-3",    "iso-8859-4",   "iso-8859-5",
    "iso-8859-6",    "iso-8859-7",   "iso-8859-8",    "iso-8859-8-i", "iso-8859-10",
    "iso-8859-13",   "iso-8859-14",  "iso-8859-15",   "iso-8859-16",  "koi8-r",
    "koi8-u",        "macintosh",    "windows-874",   "windows-1250", "windows-1251",
    "windows-1252",  "windows-1253", "windows-1254",  "windows-1255", "windows-1256",
    "windows-1257",  "windows-1258", "x-mac-cyrillic",
};

/* Reads index-<name>.txt: pointer<TAB>code point per line. Missing pointers are U+FFFD. */
static void load_index(const char *name, uint32_t table[128], bool *ok)
{
    char path[512];
    snprintf(path, sizeof path, EMBER_SOURCE_DIR "/tools/unicodegen/data/encoding/index-%s.txt", name);
    size_t size = 0;
    char *text = SDL_LoadFile(path, &size);
    *ok = text != nullptr;
    for (int i = 0; i < 128; i++) {
        table[i] = 0xFFFD;
    }
    if (text == nullptr) {
        return;
    }
    size_t i = 0;
    while (i < size) {
        size_t e = i;
        while (e < size && text[e] != '\n') {
            e++;
        }
        if (text[i] != '#' && text[i] != '\r' && i < e) {
            /* "  <decimal pointer><TAB><0x hex code point><TAB>name" with leading blanks */
            size_t k = i;
            uint32_t ptr = 0;
            uint32_t cp = 0;
            bool any = false;
            while (k < e && (text[k] == ' ' || text[k] == '\t')) {
                k++;
            }
            while (k < e && text[k] >= '0' && text[k] <= '9') {
                ptr = ptr * 10 + (uint32_t)(text[k] - '0');
                k++;
                any = true;
            }
            while (k < e && (text[k] == '\t' || text[k] == ' ')) {
                k++;
            }
            bool hex = false;
            if (k + 1 < e && text[k] == '0' && (text[k + 1] == 'x' || text[k + 1] == 'X')) {
                k += 2;
            }
            while (k < e && ((text[k] >= '0' && text[k] <= '9') || (text[k] >= 'a' && text[k] <= 'f') ||
                             (text[k] >= 'A' && text[k] <= 'F'))) {
                uint32_t d = (uint32_t)(text[k] <= '9' ? text[k] - '0' : (text[k] | 0x20) - 'a' + 10);
                cp = cp * 16 + d;
                k++;
                hex = true;
            }
            if (any && hex && ptr < 128) {
                table[ptr] = cp;
            }
        }
        i = e + 1;
    }
    SDL_free(text);
}

/* Decodes one byte with an encoding and returns the single code point it produces. */
static uint32_t decode_one(const embi_encoding *enc, unsigned char b)
{
    embi_decoder d;
    embi_decoder_init(&d, enc, false, false);
    embi_sb out = {nullptr, 0, 0};
    (void)embi_decode(&d, &b, 1, true, &out);
    const char *p = out.data;
    const char *e = out.data != nullptr ? out.data + out.len : nullptr;
    uint32_t cp = p != nullptr && p < e ? embi_utf8_decode(&p, e) : 0xFFFFFFFFu;
    embi_sb_free(&out);
    return cp;
}

EMB_TEST(encoding_single_byte_tables_match_index_files)
{
    size_t checked = 0;
    for (size_t n = 0; n < sizeof single_byte / sizeof single_byte[0]; n++) {
        const embi_encoding *enc = embi_encoding_named(single_byte[n]);
        EMB_REQUIRE(enc != nullptr);
        EMB_CHECK(enc->kind == EMBI_ENC_SINGLE);
        uint32_t table[128];
        bool loaded = false;
        load_index(strcmp(single_byte[n], "iso-8859-8-i") == 0 ? "iso-8859-8" : single_byte[n], table,
                   &loaded);
        EMB_REQUIRE(loaded);
        for (unsigned b = 0; b < 0x80; b++) {
            EMB_CHECK_EQ_INT((int)decode_one(enc, (unsigned char)b), (int)b);
        }
        for (unsigned b = 0x80; b < 0x100; b++) {
            uint32_t want = table[b - 0x80];
            uint32_t got = decode_one(enc, (unsigned char)b);
            if (got != want) {
                fprintf(stderr, "  %s byte 0x%02X: got U+%04X want U+%04X\n", single_byte[n], b, got, want);
            }
            EMB_CHECK(got == want);
            checked++;
        }
    }
    EMB_CHECK(checked == 28 * 128);
}

/* Encodes one code point; returns the byte count (0 on failure) and the bytes in b. */
static size_t encode_one(const embi_encoding *enc, uint32_t cp, char b[8])
{
    char u8[4];
    size_t k = embi_utf8_encode(cp, u8);
    embi_sb out = {nullptr, 0, 0};
    bool ok = embi_encode(enc, (embi_str){u8, (uint32_t)k}, false, &out);
    size_t n = ok ? out.len : 0;
    if (ok && n <= 8) {
        memcpy(b, out.data, n);
    }
    embi_sb_free(&out);
    return n;
}

EMB_TEST(encoding_single_byte_encoders_match_index_files)
{
    for (size_t n = 0; n < sizeof single_byte / sizeof single_byte[0]; n++) {
        const embi_encoding *enc = embi_encoding_named(single_byte[n]);
        EMB_REQUIRE(enc != nullptr);
        uint32_t table[128];
        bool loaded = false;
        load_index(strcmp(single_byte[n], "iso-8859-8-i") == 0 ? "iso-8859-8" : single_byte[n], table,
                   &loaded);
        EMB_REQUIRE(loaded);
        char b[8];
        for (uint32_t c = 0; c < 0x80; c++) {
            EMB_CHECK(encode_one(enc, c, b) == 1 && (uint8_t)b[0] == c);
        }
        for (unsigned i = 0; i < 128; i++) {
            uint32_t cp = table[i];
            if (cp == 0xFFFD) {
                /* A hole in the index: the byte decodes to U+FFFD and nothing encodes to it. */
                EMB_CHECK(encode_one(enc, cp, b) == 0);
                continue;
            }
            /* The encoder returns the first pointer holding the code point, and it decodes back. */
            unsigned first = i;
            for (unsigned j = 0; j < i; j++) {
                if (table[j] == cp) {
                    first = j;
                    break;
                }
            }
            EMB_CHECK(encode_one(enc, cp, b) == 1);
            EMB_CHECK((uint8_t)b[0] == 0x80 + first);
            EMB_CHECK(decode_one(enc, (unsigned char)b[0]) == cp);
        }
    }
}

EMB_TEST(encoding_encoder_errors_and_other_kinds)
{
    const embi_encoding *w = embi_encoding_named("windows-1252");
    embi_sb out = {nullptr, 0, 0};
    /* Unmappable code points: failure, or a decimal reference in the HTML error mode. */
    EMB_CHECK(!embi_encode(w, EMBI_STR("a\xE2\x82\xAC" "b\xE4\xBD\xA0" "c"), false, &out));
    EMB_CHECK(out.len == 3 && out.data[0] == 'a' && (uint8_t)out.data[1] == 0x80 && out.data[2] == 'b');
    embi_sb_clear(&out);
    EMB_CHECK(embi_encode(w, EMBI_STR("a\xE4\xBD\xA0" "b"), true, &out));
    EMB_CHECK(out.len == 10 && memcmp(out.data, "a&#20320;b", 10) == 0);
    embi_sb_clear(&out);

    const embi_encoding *u = embi_encoding_named("utf-8");
    EMB_CHECK(embi_encode(u, EMBI_STR("h\xC3\xA9"), false, &out));
    EMB_CHECK(out.len == 3);
    embi_sb_clear(&out);

    const embi_encoding *x = embi_encoding_named("x-user-defined");
    EMB_REQUIRE(x != nullptr);
    EMB_CHECK(embi_encode(x, EMBI_STR("a\xEF\x9E\x80"), false, &out)); /* U+F780 */
    EMB_CHECK(out.len == 2 && (uint8_t)out.data[1] == 0x80);
    embi_sb_clear(&out);
    EMB_CHECK(!embi_encode(x, EMBI_STR("\xC3\xA9"), false, &out));
    embi_sb_clear(&out);

    const embi_encoding *sj = embi_encoding_for_label(EMBI_STR("shift_jis"));
    EMB_REQUIRE(sj != nullptr);
    EMB_CHECK(!embi_encode(sj, EMBI_STR("a"), false, &out));
    EMB_CHECK(out.len == 0);
    embi_sb_free(&out);
}

EMB_TEST(encoding_labels_resolve_as_the_standard_lists_them)
{
    const embi_encoding *e = embi_encoding_for_label(EMBI_STR(" UTF-8 "));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(e->kind == EMBI_ENC_UTF8);
    EMB_CHECK(strcmp(e->name, "utf-8") == 0);

    e = embi_encoding_for_label(EMBI_STR("latin1"));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(strcmp(e->name, "windows-1252") == 0);

    e = embi_encoding_for_label(EMBI_STR("ISO-8859-8-I"));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(strcmp(e->name, "iso-8859-8-i") == 0);
    EMB_CHECK(e->kind == EMBI_ENC_SINGLE);

    e = embi_encoding_for_label(EMBI_STR("x-user-defined"));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(e->kind == EMBI_ENC_USER_DEFINED);

    e = embi_encoding_for_label(EMBI_STR("unicodefeff"));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(e->kind == EMBI_ENC_UTF16LE);

    /* CJK encodings are recognized but flagged, so the caller can report them. */
    e = embi_encoding_for_label(EMBI_STR("shift_jis"));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(e->kind == EMBI_ENC_UNSUPPORTED);
    e = embi_encoding_for_label(EMBI_STR("gbk"));
    EMB_REQUIRE(e != nullptr);
    EMB_CHECK(e->kind == EMBI_ENC_UNSUPPORTED);

    EMB_CHECK(embi_encoding_for_label(EMBI_STR("bogus")) == nullptr);
    EMB_CHECK(embi_encoding_for_label(EMBI_STR("   ")) == nullptr);
    EMB_CHECK(embi_encoding_label_name((const unsigned char *)"cp1252", 6) != nullptr);
    EMB_CHECK(strcmp(embi_encoding_label_name((const unsigned char *)"cp1252", 6), "windows-1252") == 0);
    EMB_CHECK(embi_encoding_label_name(nullptr, 0) == nullptr);
}

LOADER_TEST_MAIN()
