#pragma once
/*
 * HTML document input decoding (W7a). The parser takes UTF-8, so a document's byte stream is
 * decoded here first with the WHATWG sniffing algorithm (BOM, transport charset, meta charset
 * prescan, windows-1252). The tokenizer and the tree builder do not change.
 */
#include "loader/loader.h"

/* Decodes a document byte stream to UTF-8 (appended to out). transport_charset is the charset from
 * the response's Content-Type, or empty. used receives the encoding chosen (never nullptr). */
void embi_html_decode_bytes(const unsigned char *p, size_t n, embi_str transport_charset,
                            embi_sb *out, const embi_encoding **used);
