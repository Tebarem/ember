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

/* Queues fn(user) on the view's event loop. This is the only call that may come from another thread: it is safe to
 * call while the view's own thread runs its loop. Returns false when the view is null. Tasks queued by the view's own
 * thread run in order with timers and input. */
EMB_API bool emb_view_post_task(emb_view *view, emb_task_fn fn, void *user);
