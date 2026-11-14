#pragma once
/*
 * Ember scripting API: evaluation for hosts, the console callback and posted tasks.
 * It is a separate header (not part of ember.h) and includes ember.h for the view and runtime types. C23 only.
 *
 * A view runs scripts in one realm per loaded document, created on the first script (a static page pays nothing).
 * Script errors are logged at EMB_LOG_ERROR with a line that starts with "uncaught exception:" (a thrown error that
 * nothing caught), "unhandled rejection:" (a rejected promise nothing handled) or "console.error:" (console.error).
 */

#include <ember/ember.h>

#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Receives the result of emb_view_eval. ok is false when the code threw; text is the completion value (or the
 * exception) as a UTF-8 string, valid only during the call. */
typedef void (*emb_eval_result_fn)(void *user, bool ok, const char *text, size_t len);

/* Evaluates js (UTF-8, len bytes, need not be NUL-terminated) as a classic script in the view's realm, then runs the
 * microtasks it queued. result (may be nullptr) is called before this returns. Returns false when the code threw or
 * the view has no document. Runs on the view's thread. */
EMB_API bool emb_view_eval(emb_view *view, const char *js, size_t len, emb_eval_result_fn result, void *user);

/* Receives console output. level maps console.debug/log/info to EMB_LOG_DEBUG/INFO, warn to WARN, error and
 * assert failures to ERROR. text is UTF-8, valid only during the call. */
typedef void (*emb_console_fn)(void *user, emb_log_level level, const char *text, size_t len);

/* Sets the console callback of the view (nullptr removes it). Output is also written to the runtime's log. */
EMB_API void emb_view_set_console_callback(emb_view *view, emb_console_fn fn, void *user);

/* A task a host posts to a view's event loop. It runs on the view's thread during the next update of the view. */
typedef void (*emb_task_fn)(void *user);

/* Queues fn(user) on the view's event loop. This and emb_view_post_host_message are the calls that may come from
 * another thread: it is safe to call while the view's own thread runs its loop. Returns false when the view is null. Tasks queued by the view's own
 * thread run in order with timers and input. */
EMB_API bool emb_view_post_task(emb_view *view, emb_task_fn fn, void *user);

/* ------------------------------------------------------------------ host channel (HX1) */

/* Receives a message a page sent with <name>.postMessage(message): message converted with ToString, as UTF-8 (not
 * NUL-terminated; valid only during the call; null when len is 0). Called synchronously on the view's thread from
 * inside the script: fn must not load a document into the view or destroy it (post a task for that). */
typedef void (*emb_host_message_fn)(void *user, const char *message, size_t len);

/* Installs the host channel of the view: every document the view loads (and the current one, at once, when its realm
 * already exists) gets a global property `name` holding an object with
 *   postMessage(message)  calls fn with String(message); throws a TypeError without an argument; returns undefined
 *   onMessage             a writable data property, null at first: the page sets it to receive the host's messages
 * The global property is read-only and not deletable (the object itself is ordinary: onMessage is assignable).
 * name is an ASCII identifier of at most 63 bytes; returns false (and changes nothing) for any other name or a null view.
 * A name that is already a global replaces it when configurable (window, document: pick a name of your own); when it
 * is not (undefined, NaN), the document gets no channel and a warning is logged.
 * Calling it again replaces fn and user (and the name, for documents loaded afterwards). fn == nullptr keeps the object in
 * documents but drops their messages. Only the view's top-level document gets the channel, not iframe documents. */
EMB_API bool emb_view_set_host_channel(emb_view *view, const char *name, emb_host_message_fn fn, void *user);

/* Sends message (UTF-8, copied) to the page: during the next update of the view, <name>.onMessage(message) is called
 * with it as a string, if the channel is installed and onMessage is callable at that time; otherwise the message is
 * dropped. Messages arrive in the order they were posted, with the view's other tasks. An exception thrown by onMessage
 * is reported like any uncaught exception (EMB_LOG_ERROR "uncaught exception: ...") and the next message is still
 * delivered. Messages still queued when the view loads another document are dropped with its other tasks. Like
 * emb_view_post_task this may be called from any thread. Returns false for a null view or message. */
EMB_API bool emb_view_post_host_message(emb_view *view, const char *message, size_t len);

#ifdef __cplusplus
}
#endif
