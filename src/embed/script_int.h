#pragma once
/*
 * Internal declarations of the script layer (W5c): the event loop (loop.c) and the scripts of
 * a document (script.c). doc.c calls them; the public API (include/ember/ember_script.h) is implemented in script.c.
 */
#include "embed/doc.h"
#include "html/parser.h"

typedef struct embi_bind_ctx embi_bind_ctx;

#include <ember/ember_script.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_loop embi_loop;
typedef struct embi_script embi_script;
typedef struct embi_element embi_element;

/* ------------------------------------------------------------------ the event loop (loop.c) */

/* What the loop calls. fire runs a timer's payload; release frees the payload of a one-shot timer after it fired (and of
 * the timers that embi_loop_clear_timers drops); checkpoint runs after each task and timer (the microtask checkpoint). */
typedef struct embi_loop_hooks {
    void *user;
    void (*fire)(void *user, void *payload);
    void (*release)(void *user, void *payload);
    void (*checkpoint)(void *user);
} embi_loop_hooks;

embi_loop *embi_loop_new(const embi_loop_hooks *hooks);
void embi_loop_free(embi_loop *l);
/* Queues fn(user) on the loop's thread. */
void embi_loop_task(embi_loop *l, void (*fn)(void *user), void *user);
/* Queues fn(user) from any thread. Returns false when fn is null. */
bool embi_loop_post(embi_loop *l, void (*fn)(void *user), void *user);
/* As embi_loop_post, and drop(user) runs instead of fn when the task is discarded unrun (embi_loop_clear_tasks, which a
 * navigation calls, or embi_loop_free), so a task that owns memory or a reference does not leak. drop may be null. */
bool embi_loop_post_ex(embi_loop *l, void (*fn)(void *user), void *user, void (*drop)(void *user));
/* The timer hooks (bind.h embi_bind_timers) with user = the loop. */
uint32_t embi_loop_timer_add(void *user, double timeout_ms, bool repeat, void *payload);
void *embi_loop_timer_clear(void *user, uint32_t id);
/* Drops every timer, releasing the payloads through the release hook. */
void embi_loop_clear_timers(embi_loop *l);
/* Drops the queued and the posted tasks that have not run, calling their drop functions. */
void embi_loop_clear_tasks(embi_loop *l);
/* Runs the posted tasks, the queued tasks and the timers due at now_ms. Returns true when anything ran. */
bool embi_loop_pump(embi_loop *l, double now_ms);
/* Whether embi_loop_post also pushes an SDL user event (a windowed view blocked in its event wait wakes up on it). */
void embi_loop_set_wake(embi_loop *l, bool wake);
/* True when a task is queued or posted and has not run (timers do not count). */
bool embi_loop_has_tasks(const embi_loop *l);
/* True when no task and no timer is pending. */
bool embi_loop_idle(const embi_loop *l);
/* The earliest due time of a live timer. Returns false when there is none. */
bool embi_loop_next_due(const embi_loop *l, double *due_ms);

/* ------------------------------------------------------------------ the scripts of a document (script.c) */

/* Creates the script state of a document (no JS yet: the realm is created on the first script). */
embi_script *embi_script_new(embi_doc *d);
void embi_script_free(embi_script *s);
/* Drops the realm, the timers and the deferred scripts, before the document's DOM is replaced or freed. */
void embi_script_reset(embi_script *s);
/* FP2a: the realm is built on a thread from embi_script_new on. A document that will have no script (scripting off, or markup
 * with no <script>) lets it go; it is freed as soon as its thread is done, never waited for. */
void embi_script_no_prewarm(embi_script *s);
void embi_script_hint_markup(embi_script *s, embi_str html);
/* True while a prewarmed realm (or the thread that builds or frees it) is still held; the pump clears it once an unwanted one is gone. */
bool embi_script_prewarm_held(const embi_script *s);
/* The parser's on_script hook (html/parser.h): runs a classic script, or defers or queues it. Returns CONTINUE; PAUSE when the script waits for the network (the load suspends);
 * STOP when the script replaced the document. */
embi_html_script_result embi_script_on_script(void *ctx, embi_element *script);
/* Whether a script (from the parser's hook or the blocked script the load runs) is running now: the DOM changes made meanwhile are
 * the script's, not the parser's. */
bool embi_script_active(const embi_script *s);
/* The view's load that waited for a running script (view.c); run when the outermost script has returned. */
void embi_view_run_deferred_load(void *view);
/* Whether the parser paused on a classic script whose source (or a stylesheet before it) is still downloading (N1f). */
bool embi_script_has_blocked(const embi_script *s);
/* Runs that script if the network has delivered what it waits for, and returns true (the caller resumes the parser); false
 * while it still waits. True when there is no such script. A script may replace the document (see embi_doc_load_begin). */
bool embi_script_run_blocked(embi_script *s);
/* Called when the parser has finished the document: starts the module preloads and makes the document interactive. */
void embi_script_parse_done(embi_script *s);
/* Runs the deferred and module scripts in document order. Returns false while one of them (or a stylesheet before it, or the
 * imports of a module) is still downloading: call it again after an arrival. True when all have run. */
bool embi_script_run_defers(embi_script *s);
/* The load is complete and laid out: queues DOMContentLoaded and load (load waits for the downloads still going on). */
void embi_script_finish_events(embi_script *s);
/* N1f: a network record completed. Answers the module requests that waited for it, runs the async scripts that are ready, and
 * lets the load event fire once nothing is downloading. url is the record's. */
void embi_script_net_arrived(embi_script *s, embi_str url);
/* Fires the gated load event if nothing is downloading any more. For an arrival that belongs to no live record (a stale one,
 * whose count still held the gate). */
void embi_script_recheck_gate(embi_script *s);
/* Runs the due tasks and timers. Returns true when something ran (the document may have changed). */
bool embi_script_pump(embi_script *s);
bool embi_script_eval(embi_script *s, const char *src, size_t len, emb_eval_result_fn result, void *user);
/* N1d: a network image arrived (ok) or failed: every <img> whose src resolves to url gets its load or error event. */
void embi_script_image_event(embi_script *s, embi_str url, bool ok);
/* The earliest due time of a live timer, for the windowed idle wait (the loop sleeps until then). False when none. */
bool embi_script_next_due(embi_script *s, double *due_ms);
/* True when a task is queued (or posted) and not yet run; timers do not count. A windowed view with no task sleeps until
 * the next event or the next timer. */
bool embi_script_has_tasks(embi_script *s);
/* CF2: embi_bind_idle on the document's realm once the document is complete, when a realm exists, no script is running and
 * no task waits (see embi_doc_idle). */
bool embi_script_idle(embi_script *s, bool anim_soon);
/* The windowed view turns the wake-up on: posted tasks push an SDL user event. */
void embi_script_set_wake(embi_script *s, bool wake);
void embi_script_set_console(embi_script *s, emb_console_fn fn, void *user);
bool embi_script_post(embi_script *s, emb_task_fn fn, void *user);
/* As embi_script_post with a drop function (see embi_loop_post_ex). */
bool embi_script_post_ex(embi_script *s, emb_task_fn fn, void *user, void (*drop)(void *user));
/* emb_view_post_task with a drop function. */
bool embi_view_post_task_ex(emb_view *view, emb_task_fn fn, void *user, void (*drop)(void *user));
/* The binding context of the document's realm, made on the first call (the input layer needs it for its events). nullptr
 * when there is no document or no memory. */
embi_bind_ctx *embi_script_bind(embi_script *s);
/* The binding context if a script made one already; never creates the realm. */
embi_bind_ctx *embi_script_bind_if(embi_script *s);
/* The document timeline's time: milliseconds since the document's time origin (what performance.now() reads). */
double embi_script_time_ms(embi_script *s);
/* FP2a: the load's script time so far: taking or building the realm, the compiler, and everything else the scripts ran (the
 * realm and compile times are not counted in run_ms). For the first-paint breakdown. */
void embi_script_stage_times(const embi_script *s, double *realm_ms, double *compile_ms, double *run_ms);
/* Routes the script layer's log lines (errors of scripts, console.error) to fn instead of the runtime's log. Tests use it. */
void embi_script_set_log(embi_script *s, void (*fn)(void *user, emb_log_level level, const char *text), void *user);

/* ------------------------------------------------------------------ accessors of the document (doc.c) */

embi_script *embi_doc_script(embi_doc *d);
embi_document *embi_doc_dom(embi_doc *d);
/* The document's URL: its base, or "about:blank" when it has none. */
embi_str embi_doc_url(embi_doc *d);
/* The view that owns the document (embi_doc_cb.owner), or nullptr for a document made without a view. */
void *embi_doc_owner(embi_doc *d);
bool embi_doc_script_running(const embi_doc *d); /* a script of the document is running (parse-time or blocked) */
/* Fetches an absolute URL through the view's loader. The bytes are SDL_malloc'd. */
bool embi_doc_fetch_abs(embi_doc *d, embi_str url, uint8_t **data, size_t *len);
/* Reads an attribute of an element (presence is the result; an empty value is present). */
bool embi_doc_el_attr(embi_element *el, const char *name, embi_sb *out);
/* The text of an element's descendants, in document order. */
void embi_doc_el_text(embi_element *el, embi_sb *out);
/* The earliest due time of a live timer of the document's loop. False when there is none. */
bool embi_doc_next_due(embi_doc *d, double *due_ms);
