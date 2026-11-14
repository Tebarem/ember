#pragma once
/*
 * Ember resource handlers v2. A separate header, not part of
 * ember.h: the v1 handler (emb_resource_fn in ember.h) stays as it is, and the v2 structs here carry
 * what v1 cannot: the request method, headers and body, and the response status, media type and
 * headers. C23 only, and it includes ember.h for the runtime type.
 *
 * Two shapes of v2 handler:
 *   - synchronous (emb_runtime_set_resource_handler_v2): fills an emb_response from the handler's
 *     call, like v1 with the extra fields. The bytes are copied before the call returns.
 *   - asynchronous (emb_runtime_set_resource_handler_async): the library calls start with the
 *     request and the handler completes it later with emb_resource_complete, from any thread. The
 *     library posts the finishing step to the requesting view's event queue, so the script-visible
 *     result (the fetch promise) is always settled on the view's thread. A cancel function is
 *     called when the requester aborts a request still in flight: the handler must still complete
 *     it once (the library then drops the result). A document load (<img>, <link>, <script>) is
 *     synchronous: an asynchronous handler serves it only when it completes inside start.
 *
 * Structs passed across the API start with a size field, as in ember.h: set it to sizeof the
 * struct. Every string in a struct is UTF-8 with a length, and need not be NUL-terminated.
 */

#include <ember/ember.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One request of an asynchronous handler. Opaque: it lives until emb_resource_complete. */
typedef struct emb_request emb_request;

/* A header name and value. */
typedef struct emb_header {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} emb_header;

/* The request a handler serves. The strings and the body are valid during the start or the
 * synchronous call; copy them to keep them. request is the handle to pass to emb_resource_complete
 * (asynchronous handlers only; nullptr for a synchronous one). */
typedef struct emb_request_info {
    uint32_t size;          /* sizeof(emb_request_info), set by the library */
    const char *method;     /* "GET", "POST", ... (as the script sent it, normalized for the standard methods) */
    size_t method_len;
    const char *url;        /* absolute, with the scheme the handler is registered for */
    size_t url_len;
    const emb_header *headers;
    size_t nheaders;
    const uint8_t *body;    /* nullptr when the request has no body */
    size_t body_len;
    emb_request *request;   /* asynchronous handlers: complete it with emb_resource_complete */
} emb_request_info;

/* The response of a handler. The library sets size and zeroes the rest before the call; the handler
 * fills what it has. status 0 means 200. mime is the Content-Type media type (nullptr for none; the
 * library adds Content-Type from it). The headers are added to the response as given. data and len
 * are the body; the bytes are copied, so the handler may reuse them afterwards. */
typedef struct emb_response {
    uint32_t size;          /* sizeof(emb_response), set by the library */
    uint16_t status;        /* set by the handler; 0 is 200 */
    const char *mime;       /* set by the handler; nullptr for none */
    size_t mime_len;
    const emb_header *headers;
    size_t nheaders;
    const uint8_t *data;    /* set by the handler */
    size_t len;
} emb_response;

/* A synchronous v2 handler. Returns false to fail the load (a network-style error for fetch). */
typedef bool (*emb_resource_v2_fn)(void *user, const emb_request_info *req, emb_response *out);
/* The start of an asynchronous v2 handler. It must not block for long: it records the request and
 * returns; the response comes later through emb_resource_complete. */
typedef void (*emb_request_start_fn)(void *user, const emb_request_info *req);
/* The cancel of an asynchronous request still in flight (the requester aborted it). */
typedef void (*emb_request_cancel_fn)(void *user, emb_request *req);

/* Registers a synchronous v2 handler for scheme (the same scheme rules as ember.h). fn == nullptr removes it.
 * Returns false for a bad scheme or for file and data. Replaces any handler of the scheme, v1 or v2. */
EMB_API bool emb_runtime_set_resource_handler_v2(emb_runtime *rt, const char *scheme, emb_resource_v2_fn fn,
                                                 void *user);
/* Registers an asynchronous handler for scheme. start == nullptr removes it. cancel may be nullptr.
 * Returns false for a bad scheme or for file and data. Replaces any handler of the scheme, v1 or v2. */
EMB_API bool emb_runtime_set_resource_handler_async(emb_runtime *rt, const char *scheme,
                                                    emb_request_start_fn start, emb_request_cancel_fn cancel,
                                                    void *user);
/* Completes an asynchronous request with resp (copied; resp->size must be set; nullptr completes it with a network
 * error). Call it exactly once per request, from any thread: the library marshals the result to the requesting view's
 * thread. The handle is invalid after this call, also when the requester aborted the request earlier (the result is
 * then dropped) and when the view or the runtime is gone (the result is dropped and the request freed). A second call
 * for the same request is ignored while the handle is still valid, but the handle must not be used after the first. */
EMB_API void emb_resource_complete(emb_request *req, const emb_response *resp);

/* Names the persistent storage of the runtime for localStorage: an SDL user storage container (org and app). Without a call, localStorage is
 * memory-only. Call it before a view runs its first script; it returns false when the runtime already has a storage
 * backend or when the container cannot be opened. */
EMB_API bool emb_runtime_set_storage(emb_runtime *rt, const char *org, const char *app);
/* The same with a plain directory as the backend (tests, tools). The directory is created when missing. */
EMB_API bool emb_runtime_set_storage_dir(emb_runtime *rt, const char *dir);

/* Serves the files of a directory under scheme://host/: the request's path, percent-decoded, names the file under
 * dir, and "/" is index.html. The media type comes from the file's extension. A missing file is a 404 response, and a
 * path with a ".." segment, a backslash or a colon is refused (a network error). Example: emb_runtime_mount_dir(rt,
 * "app", "local", "dist") serves app://local/index.html from dist/index.html. The scheme is one handler: mounting it
 * replaces a handler the embedder registered for it. Returns false for a bad scheme or host, for file, data and blob, or
 * when the directory is empty or too long. The runtime holds its own copy of the strings. */
EMB_API bool emb_runtime_mount_dir(emb_runtime *rt, const char *scheme, const char *host, const char *dir);

#ifdef __cplusplus
}
#endif
