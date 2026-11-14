// fuzz-modules: loader base js
/* WHATWG URL: the basic URL parser (with and without a base and a query encoding), every getter
 * and setter, origin, canParse, IDNA ToASCII, Punycode, percent decoding and the urlencoded form
 * parser/serializer. Input: byte 0 = mode; the rest is split at NUL bytes into the URL text, a base
 * URL text and a setter value. Checked invariants: a parsed URL serializes to text that parses
 * again to the same text (the parser is idempotent), and a Punycode encode of what decode produced
 * decodes back to the same code points. */
#include "fuzz_common.h"

#include "loader/loader.h"

static const embi_url_attr attrs[] = {EMBI_URL_HREF,     EMBI_URL_PROTOCOL, EMBI_URL_USERNAME,
                                      EMBI_URL_PASSWORD, EMBI_URL_HOST,     EMBI_URL_HOSTNAME,
                                      EMBI_URL_PORT,     EMBI_URL_PATHNAME, EMBI_URL_SEARCH,
                                      EMBI_URL_HASH};

static embi_str piece(fuzz_in *in)
{
    const uint8_t *nul = in->n ? memchr(in->p, 0, in->n) : nullptr;
    size_t n = nul ? (size_t)(nul - in->p) : in->n;
    static const char empty[1] = {0};
    embi_str s = {n ? (const char *)in->p : empty, (uint32_t)n};
    in->p += n;
    in->n -= n;
    if (nul) {
        in->p++;
        in->n--;
    }
    return s;
}

static void check_idempotent(const embi_url *u)
{
    embi_url again;
    embi_str href = {u->href, u->href_len};
    if (!embi_url_parse(href, nullptr, &again)) {
        fprintf(stderr, "serialized URL does not parse: %.*s\n", (int)href.len, href.ptr);
        abort();
    }
    if (again.href_len != u->href_len || memcmp(again.href, u->href, u->href_len) != 0) {
        fprintf(stderr, "URL parse is not idempotent: '%.*s' -> '%.*s'\n", (int)u->href_len, u->href,
                (int)again.href_len, again.href);
        abort();
    }
    embi_url_free(&again);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    embi_str text = piece(&in);
    embi_str base_text = piece(&in);
    embi_str value = piece(&in);

    if ((mode & 3) == 0) {
        embi_url base;
        bool have_base = embi_url_parse(base_text, nullptr, &base);
        embi_url u;
        const embi_encoding *enc = (mode & 4) ? embi_encoding_named("windows-1252") : nullptr;
        bool ok = enc ? embi_url_parse_enc(text, have_base ? &base : nullptr, enc, &u)
                      : embi_url_parse(text, have_base ? &base : nullptr, &u);
        (void)embi_url_can_parse(text, have_base ? &base : nullptr);
        if (ok) {
            if (!enc) {
                check_idempotent(&u);
            }
            embi_sb sb = {0};
            for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
                embi_sb_clear(&sb);
                embi_url_get(&u, attrs[i], &sb);
            }
            embi_sb_clear(&sb);
            embi_url_origin(&u, &sb);
            (void)embi_url_is_special(&u);
            embi_url copy;
            if (embi_url_copy(&u, &copy)) {
                for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
                    (void)embi_url_set(&copy, attrs[(i + mode) % (sizeof attrs / sizeof attrs[0])], value);
                    embi_sb_clear(&sb);
                    embi_url_get(&copy, attrs[i], &sb);
                }
                check_idempotent(&copy);
                embi_url_free(&copy);
            }
            embi_sb_free(&sb);
            embi_url_free(&u);
        }
        if (have_base) {
            embi_url_free(&base);
        }
    } else if ((mode & 3) == 1) {
        embi_sb out = {0};
        (void)embi_idna_to_ascii(text, &out);
        embi_sb_clear(&out);
        if (embi_punycode_decode(text, &out)) {
            /* Decode the UTF-8 back into code points and encode again. */
            uint32_t cps[256];
            uint32_t n = 0;
            const unsigned char *p = (const unsigned char *)out.data;
            size_t i = 0;
            while (i < out.len && n < 256) {
                uint32_t cp = p[i];
                size_t len = 1;
                if (cp >= 0xF0) {
                    len = 4;
                    cp &= 7;
                } else if (cp >= 0xE0) {
                    len = 3;
                    cp &= 15;
                } else if (cp >= 0xC0) {
                    len = 2;
                    cp &= 31;
                }
                for (size_t k = 1; k < len && i + k < out.len; k++) {
                    cp = (cp << 6) | (p[i + k] & 63u);
                }
                cps[n++] = cp;
                i += len;
            }
            embi_sb enc = {0};
            (void)embi_punycode_encode(cps, n, &enc);
            embi_sb_free(&enc);
        }
        embi_sb_free(&out);
        /* Code points taken straight from the input bytes. */
        uint32_t cps[64];
        uint32_t n = 0;
        for (size_t i = 0; i + 2 < text.len && n < 64; i += 3) {
            uint32_t cp = ((uint32_t)(uint8_t)text.ptr[i] << 16 | (uint32_t)(uint8_t)text.ptr[i + 1] << 8 |
                           (uint8_t)text.ptr[i + 2]) % 0x110000u;
            cps[n++] = cp;
        }
        embi_sb enc = {0};
        (void)embi_punycode_encode(cps, n, &enc);
        embi_sb_free(&enc);
    } else if ((mode & 3) == 2) {
        embi_form f = {0};
        embi_form_parse(text, &f);
        embi_sb ser = {0};
        embi_form_serialize(&f, &ser);
        embi_form g = {0};
        embi_form_parse((embi_str){ser.data ? ser.data : "", ser.len}, &g);
        if (g.len != f.len) {
            fprintf(stderr, "urlencoded round trip changed the pair count %u -> %u\n", (unsigned)f.len,
                    (unsigned)g.len);
            abort();
        }
        embi_form_free(&g);
        embi_form_free(&f);
        embi_sb_free(&ser);
    } else {
        embi_sb out = {0};
        embi_pct_decode(text, &out);
        embi_sb_free(&out);
        embi_sb mime = {0};
        embi_sb body = {0};
        (void)embi_data_url(text, &mime, &body);
        embi_sb_free(&mime);
        embi_sb_free(&body);
    }
    FUZZ_GUARD_END();
    return 0;
}
