/*
 * HTML document input decoding: the W7a hook in front of the parser (see input.h). Only the byte
 * to UTF-8 step lives here; the parser entry point is unchanged.
 */
#include "html/input.h"

void embi_html_decode_bytes(const unsigned char *p, size_t n, embi_str transport_charset,
                            embi_sb *out, const embi_encoding **used)
{
    embi_decode_document(p, n, transport_charset, out, used);
}
