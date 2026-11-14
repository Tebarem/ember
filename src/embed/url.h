#pragma once
/*
 * URL helpers of the embedding layer (W4e): resolution against a base (RFC 3986 section 5.2), file: paths, and
 * the data: scheme. Pure string code: no GPU, no style, no layout headers, so any translation unit can use it.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The scheme of url ("name:" with at least two characters, so a Windows drive letter is not a scheme). The
 * scheme text is returned without the colon. */
bool embi_url_scheme(embi_str url, embi_str *scheme);

/* Resolves rel against base. base may be empty (rel is then returned as it is). The result is SDL_malloc'd and
 * NUL-terminated; *len receives its length. Free it with SDL_free. Never returns nullptr (out of memory is fatal
 * in embi_alloc, and SDL_malloc failures are not expected here). */
char *embi_url_resolve(embi_str base, embi_str rel, size_t *len);

/* The filesystem path of a file: URL (percent-decoded, forward slashes, "C:/..." for a drive) or of a bare path
 * (no scheme). Writes at most cap - 1 bytes plus a NUL. False when url is another scheme, has a non-local host,
 * or contains a ".." segment or an encoded NUL or slash problem. *len is the path length. */
bool embi_url_to_path(embi_str url, char *out, size_t cap, size_t *len);

/* The file: URL of a path (drive letters and backslashes accepted). Writes at most cap - 1 bytes plus a NUL and
 * returns the length the URL needs. */
size_t embi_url_from_path(embi_str path, char *out, size_t cap);
/* As embi_url_from_path, but a relative path is first resolved against the current directory (SDL), so the URL is
 * the file's real location and not a path at the root of the file system. */
size_t embi_url_from_bare_path(embi_str path, char *out, size_t cap);

/* Decodes a data: URL into SDL_malloc'd bytes (*data, *len). The media type is ignored. False for a malformed URL
 * or a non-data scheme. */
bool embi_url_data(embi_str url, uint8_t **data, size_t *len);

/* ASCII lowercase comparison of a scheme with name. */
bool embi_url_scheme_is(embi_str scheme, const char *name);
