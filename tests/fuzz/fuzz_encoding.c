// fuzz-modules: loader html dom base js
/* The Encoding Standard: label lookup, the streaming decoder for every encoding (fatal and
 * replacement modes, BOM handling, split reads), the encoder, the document sniffing algorithm and
 * the HTML input decoder. Input: byte 0 = encoding (index into the list of all canonical names),
 * byte 1: bit 0 fatal, bit 1 ignore BOM, bit 2 html error mode for the encoder, bits 3..7 = read
 * chunk size - 1; rest = bytes. Checked invariant: a non-fatal decoder only writes well-formed UTF-8
 * (and never a surrogate), whatever the input and chunking. */
#include "fuzz_common.h"

#include "html/input.h"
#include "loader/loader.h"

static const char *const names[] = {
    "utf-8", "ibm866", "iso-8859-2", "iso-8859-3", "iso-8859-4", "iso-8859-5", "iso-8859-6",
    "iso-8859-7", "iso-8859-8", "iso-8859-8-i", "iso-8859-10", "iso-8859-13", "iso-8859-14",
    "iso-8859-15", "iso-8859-16", "koi8-r", "koi8-u", "macintosh", "windows-874", "windows-1250",
    "windows-1251", "windows-1252", "windows-1253", "windows-1254", "windows-1255", "windows-1256",
    "windows-1257", "windows-1258", "x-mac-cyrillic", "gbk", "gb18030", "big5", "euc-jp",
    "iso-2022-jp", "shift_jis", "euc-kr", "replacement", "utf-16be", "utf-16le", "x-user-defined",
};

static bool valid_utf8(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t which = fuzz_u8(&in);
    uint8_t opts = fuzz_u8(&in);
    const embi_encoding *enc = embi_encoding_named(names[which % (sizeof names / sizeof names[0])]);
    if (!enc) {
        abort();
    }
    bool fatal = opts & 1;
    size_t chunk = (size_t)(opts >> 3) + 1;

    embi_decoder d;
    embi_decoder_init(&d, enc, fatal, (opts & 2) != 0);
    embi_sb out = {0};
    size_t pos = 0;
    bool ok = true;
    while (pos < in.n) {
        size_t take = in.n - pos < chunk ? in.n - pos : chunk;
        bool last = pos + take == in.n;
        if (!embi_decode(&d, in.p + pos, take, last, &out)) {
            ok = false;
            break;
        }
        pos += take;
    }
    if (in.n == 0) {
        ok = embi_decode(&d, nullptr, 0, true, &out);
    }
    if (!fatal && ok && !valid_utf8((const unsigned char *)out.data, out.len)) {
        fprintf(stderr, "decoder %s wrote malformed UTF-8\n", enc->name);
        abort();
    }

    /* Encode what the decoder produced (well-formed UTF-8) back, in both error modes. */
    if (out.len > 0 && valid_utf8((const unsigned char *)out.data, out.len)) {
        embi_sb enc_out = {0};
        (void)embi_encode(enc, (embi_str){out.data, out.len}, (opts & 4) != 0, &enc_out);
        embi_sb_free(&enc_out);
    }
    uint8_t byte;
    for (uint32_t cp = 0; cp < 4 && cp < in.n; cp++) {
        (void)embi_encode_single(enc, in.p[cp] * 257u, &byte);
    }

    /* Sniffing and the HTML input decoder, with the first bytes as a transport label too. */
    size_t lab = in.n < 12 ? in.n : 12;
    embi_str label = {lab ? (const char *)in.p : "", (uint32_t)lab};
    (void)embi_encoding_for_label(label);
    (void)embi_encoding_label_name(in.p, lab);
    (void)embi_sniff_encoding(in.p, in.n, label);
    (void)embi_sniff_encoding_fb(in.p, in.n, label, enc);
    embi_sb doc = {0};
    const embi_encoding *used = nullptr;
    embi_decode_document(in.p, in.n, label, &doc, &used);
    embi_sb_free(&doc);
    embi_sb doc2 = {0};
    embi_html_decode_bytes(in.p, in.n, EMBI_STR(""), &doc2, &used);
    embi_sb_free(&doc2);
    embi_sb doc3 = {0};
    embi_decode_document_fb(in.p, in.n, EMBI_STR(""), enc, &doc3, &used);
    embi_sb_free(&doc3);

    embi_sb_free(&out);
    FUZZ_GUARD_END();
    return 0;
}
