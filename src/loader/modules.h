#pragma once
/*
 * Module host (W7d): the JS engine's module loader driven by the loader's URL parser, the
 * HTML module rules and the import maps. One host per document (the realm of the document), made by the script layer
 * (src/embed/script.c), which supplies the loads: a synchronous fetch that returns the media type with the bytes.
 *
 * Module identity is the absolute URL (the engine keeps one record per key and type). A module's source is requested
 * once: the host remembers the URLs it has completed, and a later request for one of them completes with no source
 * (the engine already holds the record). Inline module scripts get a key of their own, the document URL with a
 * "#ember-inline-N" fragment, so their relative imports resolve against the document and import.meta.url is the
 * document URL.
 *
 * Internal API: embi_ prefix. Includes the JS headers (the module host is the JS-facing part of the loader).
 */
#include "base/base.h"
#include "js/js.h"

typedef struct embi_modules embi_modules;

/* What the host needs from the document. load fetches an absolute URL synchronously: on success the bytes are
 * SDL_malloc'd (returned in data) and the response's media type is appended to mime (empty when the load gives none).
 * The host decides from the media type and the extension what the module is. */
typedef struct embi_mod_host {
    void *user;
    bool (*load)(void *user, embi_str url, uint8_t **data, size_t *len, embi_sb *mime);
    /* A line for the log (warnings: an import map that is ignored, a failed load). */
    void (*warn)(void *user, const char *text);
    /* Optional (N1d): url (absolute) will probably be needed soon: a static import of a module that has just arrived. The
     * host may start downloading it. */
    void (*prefetch)(void *user, embi_str url);
    /* Optional (N1f): whether a load that just failed is only not finished yet (a network module still downloading). The
     * engine's request then waits, and embi_modules_retry answers it when the host has the module (or knows it failed). */
    bool (*is_pending)(void *user, embi_str url);
} embi_mod_host;

/* Creates the host for realm (the document's realm, which the host's native functions live in). The host is installed
 * on the runtime by embi_modules_attach. */
embi_modules *embi_modules_new(embi_js_runtime *rt, embi_js_realm *realm, const embi_mod_host *host);
/* Frees the host's records. Call it after the runtime is freed (no JS runs after that). */
void embi_modules_free(embi_modules *m);
/* Installs the loader and the import.meta hook on the runtime. */
void embi_modules_attach(embi_modules *m);

/* Sets the document URL (absolute; empty when unknown). Relative specifiers of the entry modules and of import() from a
 * script resolve against it. */
void embi_modules_set_doc_url(embi_modules *m, embi_str url);
/* Sets the URL of the classic script that is running (import() in it resolves against it). Empty clears it. */
void embi_modules_set_script_url(embi_modules *m, embi_str url);

/* Parses one import map (the text of a script element of type importmap) and merges it into the host's maps. Returns
 * false with a message in err (appended) when the map is invalid or comes after a module was loaded: it is then ignored
 * as a whole. */
bool embi_modules_add_importmap(embi_modules *m, embi_str text, embi_sb *err);

/* Answers the engine requests that were waiting for a download (see is_pending): those whose module has arrived load, those
 * that failed are rejected, the others keep waiting. The caller runs the microtasks afterwards. */
void embi_modules_retry(embi_modules *m);
/* Whether any engine request is waiting for a download. */
bool embi_modules_pending(const embi_modules *m);

/* Fetches a module ahead of its import (<link rel="modulepreload">): the bytes are kept until the module is requested.
 * Returns false when the load fails. */
bool embi_modules_preload(embi_modules *m, embi_str url);

/* Evaluates the module at url (absolute). Returns its evaluation promise (the engine's). */
embi_js_value embi_modules_eval_url(embi_modules *m, embi_str url);
/* Evaluates an inline module script: src is its text, and the module's URL is the document's. Returns the promise. */
embi_js_value embi_modules_eval_source(embi_modules *m, embi_str src);

/* Resolves a module specifier against base (absolute, may be empty) through the import maps. The result is appended
 * to out. False with a message appended to err when the specifier cannot be resolved (a bare specifier with no map
 * entry: a TypeError in the script). */
bool embi_modules_resolve(embi_modules *m, embi_str specifier, embi_str base, embi_sb *out, embi_sb *err);
