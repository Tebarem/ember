/*
 * MessageChannel, MessagePort, MessageEvent and Window.postMessage (W7f): delivery as one task per message after the
 * microtasks, start() and onmessage, close, transfer of ports and of a ReadableStream, event.ports, targetOrigin.
 *
 * The test context has a manual task queue behind the loader environment's post hook; settle_tasks() runs the queued tasks
 * in order with a microtask checkpoint after each, as the view's loop does.
 */
#include "loader_env.h"

typedef struct ptask {
    void (*fn)(void *);
    void (*drop)(void *);
    void *arg;
} ptask;

static ptask g_q[512];
static int g_qn;
static int g_qhead;

static bool test_post(void *user, void (*fn)(void *), void (*drop)(void *), void *arg)
{
    (void)user;
    if (g_qn >= (int)(sizeof g_q / sizeof g_q[0])) {
        return false;
    }
    g_q[g_qn++] = (ptask){fn, drop, arg};
    return true;
}

static void msg_env_init(env *e)
{
    env_init(e);
    g_qn = 0;
    g_qhead = 0;
    embi_loader_env le = {0};
    le.user = &e->net;
    le.transport.user = &e->net;
    le.transport.start = fake_start;
    le.transport.abort = fake_abort;
    le.post = test_post;
    embi_loader_attach(e->c, &le);
}

static void msg_env_done(env *e)
{
    for (int i = g_qhead; i < g_qn; i++) {
        g_q[i].drop(g_q[i].arg);
    }
    g_qn = g_qhead = 0;
    env_done(e);
}

/* Microtasks, then one task and a checkpoint, until the queue is empty. */
static void settle_tasks(env *e)
{
    embi_js_run_jobs(e->rt);
    int guard = 0;
    while (g_qhead < g_qn && guard++ < 1000) {
        ptask t = g_q[g_qhead++];
        t.fn(t.arg);
        embi_js_run_jobs(e->rt);
    }
    settle(e);
}

EMB_TEST(channel_delivers_a_clone_as_a_task)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { got: [] }; var ch = new MessageChannel(); var src = { a: 1, d: new Date(5), m: new Map([[1, 2]]) };"
                         "ch.port2.onmessage = function (ev) { R.got.push(ev); };"
                         "ch.port1.postMessage(src); R.sync = R.got.length;"));
    EMB_CHECK(js_true(&e, "R.sync === 0"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.got.length === 1 && R.got[0] instanceof MessageEvent && R.got[0].type === 'message'"));
    EMB_CHECK(js_true(&e, "R.got[0].data !== src && R.got[0].data.a === 1 && R.got[0].data.d.getTime() === 5 && R.got[0].data.m.get(1) === 2"));
    EMB_CHECK(js_true(&e, "R.got[0].target === ch.port2 && R.got[0].isTrusted === true && R.got[0].origin === '' && R.got[0].source === null && R.got[0].ports.length === 0"));
    EMB_CHECK(js_true(&e, "ch.port1 instanceof MessagePort && ch.port1 instanceof EventTarget && Object.prototype.toString.call(ch) === '[object MessageChannel]' && ch.port1 === ch.port1"));
    msg_env_done(&e);
}

EMB_TEST(one_task_per_message_in_order_after_microtasks)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { log: [] }; var ch = new MessageChannel();"
                         "ch.port1.onmessage = function (ev) { R.log.push('m' + ev.data); Promise.resolve().then(function () { R.log.push('u' + ev.data); }); };"
                         "ch.port2.postMessage(1); ch.port2.postMessage(2); ch.port2.postMessage(3);"
                         "Promise.resolve().then(function () { R.log.push('micro'); }); R.log.push('sync');"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.log.join() === 'sync,micro,m1,u1,m2,u2,m3,u3'"));
    msg_env_done(&e);
}

EMB_TEST(react_scheduler_pattern)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { log: [], n: 0 }; var channel = new MessageChannel(); var port = channel.port2;"
                         "channel.port1.onmessage = function () { R.n++; R.log.push('work' + R.n); if (R.n < 3) { port.postMessage(null); } };"
                         "port.postMessage(null); R.log.push('scheduled');"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.log.join() === 'scheduled,work1,work2,work3'"));
    msg_env_done(&e);
}

EMB_TEST(start_is_needed_for_listeners_but_not_for_onmessage)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { l: 0, h: 0 }; var ch = new MessageChannel();"
                         "ch.port2.addEventListener('message', function () { R.l++; });"
                         "ch.port1.postMessage('a'); ch.port1.postMessage('b');"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.l === 0"));
    EMB_CHECK(js_run(&e, "ch.port2.start(); ch.port2.start();"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.l === 2"));
    EMB_CHECK(js_run(&e, "var c2 = new MessageChannel(); c2.port1.postMessage(1); c2.port2.onmessage = function () { R.h++; };"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.h === 1 && typeof c2.port2.onmessage === 'function'"));
    EMB_CHECK(js_run(&e, "c2.port2.onmessage = null;"));
    EMB_CHECK(js_true(&e, "c2.port2.onmessage === null"));
    msg_env_done(&e);
}

EMB_TEST(close_stops_delivery_and_postMessage_is_a_no_op)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { n: 0 }; var ch = new MessageChannel(); ch.port2.onmessage = function () { R.n++; };"
                         "ch.port1.postMessage(1); ch.port2.close(); ch.port1.postMessage(2); ch.port2.postMessage(3); ch.port1.close();"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.n === 0"));
    msg_env_done(&e);
}

EMB_TEST(uncloneable_message_throws_data_clone_error)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_true(&e, "var ch = new MessageChannel(); (function () { try { ch.port1.postMessage(function () {}); } catch (x) { return x instanceof DOMException && x.name === 'DataCloneError'; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { ch.port1.postMessage(1, [ch.port1]); } catch (x) { return x instanceof DOMException && x.name === 'DataCloneError'; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { ch.port1.postMessage(1, [{}]); } catch (x) { return x instanceof DOMException && x.name === 'DataCloneError'; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { ch.port1.postMessage(1, 5); } catch (x) { return x instanceof TypeError; } return false; })()"));
    msg_env_done(&e);
}

EMB_TEST(a_port_transfers_with_its_queue_and_the_source_is_detached)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { got: [] }; var ch = new MessageChannel(); ch.port1.postMessage('queued-before');"
                         "var copy = structuredClone(ch.port2, { transfer: [ch.port2] });"
                         "R.isPort = copy instanceof MessagePort && copy !== ch.port2;"
                         "ch.port1.onmessage = function (ev) { R.got.push('p1:' + ev.data); };"
                         "copy.onmessage = function (ev) { R.got.push('copy:' + ev.data); copy.postMessage('echo'); };"
                         "ch.port1.postMessage('after');"
                         "ch.port2.postMessage('dead'); R.again = (function () { try { structuredClone(ch.port2, { transfer: [ch.port2] }); } catch (x) { return x.name; } return 'no error'; })();"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.isPort === true && R.again === 'DataCloneError'"));
    EMB_CHECK(js_true(&e, "R.got.join() === 'copy:queued-before,copy:after,p1:echo,p1:echo'"));
    msg_env_done(&e);
}

EMB_TEST(event_ports_carries_transferred_ports)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var outer = new MessageChannel(); var inner = new MessageChannel();"
                         "outer.port2.onmessage = function (ev) { R.n = ev.ports.length; R.frozen = Object.isFrozen(ev.ports); R.isPort = ev.ports[0] instanceof MessagePort;"
                         "  R.same = ev.ports === ev.ports; ev.ports[0].postMessage('via the transferred port'); };"
                         "inner.port1.onmessage = function (ev) { R.inner = ev.data; };"
                         "outer.port1.postMessage('hello', [inner.port2]); R.detached = inner.port2.postMessage('x') === undefined;"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.n === 1 && R.frozen === true && R.isPort === true && R.same === true && R.inner === 'via the transferred port'"));
    EMB_CHECK(js_run(&e, "var R2 = {}; var c3 = new MessageChannel(); c3.port2.onmessage = function (ev) { R2.n = ev.ports.length; };"
                         "var c4 = new MessageChannel(); c3.port1.postMessage(0, { transfer: [c4.port1] });"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R2.n === 1"));
    msg_env_done(&e);
}

EMB_TEST(array_buffer_transfer_through_a_port)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var ch = new MessageChannel(); var buf = new Uint8Array([1, 2, 3]).buffer;"
                         "ch.port2.onmessage = function (ev) { R.len = ev.data.byteLength; };"
                         "ch.port1.postMessage(buf, [buf]); R.after = buf.byteLength;"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.len === 3 && R.after === 0"));
    msg_env_done(&e);
}

EMB_TEST(a_closed_port_can_be_transferred_and_stays_silent)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { err: 0, got: 0, n: -1 }; var ch = new MessageChannel(); ch.port1.close();"
                         "var copy = structuredClone(ch.port1, { transfer: [ch.port1] });"
                         "R.isPort = copy instanceof MessagePort && copy !== ch.port1;"
                         "copy.onmessage = function () { R.got++; }; copy.onmessageerror = function () { R.err++; };"
                         "copy.postMessage('x'); ch.port2.postMessage('y');"
                         "var c2 = new MessageChannel(); var c3 = new MessageChannel(); c3.port1.close();"
                         "c2.port2.onmessage = function (ev) { R.n = ev.ports.length; R.p = ev.ports[0] instanceof MessagePort; ev.ports[0].onmessage = function () { R.got++; }; };"
                         "c2.port2.onmessageerror = function () { R.err++; };"
                         "c2.port1.postMessage('m', [c3.port1]); c3.port2.postMessage('z');"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.isPort === true && R.err === 0 && R.got === 0 && R.n === 1 && R.p === true"));
    msg_env_done(&e);
}

EMB_TEST(window_postMessage_fires_a_message_event_at_the_window)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { got: [] }; window.addEventListener('message', function (ev) { R.got.push(ev); });"
                         "window.onmessage = function (ev) { R.viaHandler = ev.data; };"
                         "window.postMessage({ k: 1 }, '*'); window.postMessage('same', '/'); window.postMessage('opts', { targetOrigin: 'http://t.test' });"
                         "window.postMessage('other', 'http://elsewhere.test'); window.postMessage('default');"
                         "R.sync = R.got.length;"));
    EMB_CHECK(js_true(&e, "R.sync === 0"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.got.map(function (x) { return typeof x.data === 'string' ? x.data : 'obj'; }).join() === 'obj,same,opts,default'"));
    EMB_CHECK(js_true(&e, "R.got[0].source === window && R.got[0].origin === 'http://t.test' && R.got[0].target === window && R.got[0].isTrusted"));
    EMB_CHECK(js_true(&e, "R.viaHandler === 'default'"));
    EMB_CHECK(js_true(&e, "(function () { try { window.postMessage('x', 'not a url'); } catch (x) { return x.name === 'SyntaxError'; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { window.postMessage(function () {}, '*'); } catch (x) { return x.name === 'DataCloneError'; } return false; })()"));
    msg_env_done(&e);
}

EMB_TEST(message_event_constructor_and_init)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_true(&e, "var ch = new MessageChannel(); var ev = new MessageEvent('message', { data: 5, origin: 'o', lastEventId: 'id', source: window, ports: [ch.port1] });"
                          "ev.data === 5 && ev.origin === 'o' && ev.lastEventId === 'id' && ev.source === window && ev.ports.length === 1 && ev.ports[0] === ch.port1 && Object.isFrozen(ev.ports)"));
    EMB_CHECK(js_true(&e, "var d = new MessageEvent('x'); d.data === null && d.origin === '' && d.source === null && d.ports.length === 0 && d instanceof Event && !d.isTrusted"));
    EMB_CHECK(js_true(&e, "var i = document.createEvent('MessageEvent'); i.initMessageEvent('message', true, false, 'd', 'o', 'l', null, []); i.type === 'message' && i.bubbles"));
    EMB_CHECK(js_true(&e, "(function () { try { new MessageEvent('x', { ports: [1] }); } catch (x) { return x instanceof TypeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new MessagePort(); } catch (x) { return x instanceof TypeError; } return false; })()"));
    msg_env_done(&e);
}

EMB_TEST(read_stream_transfers_through_structuredClone)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { chunks: [] };"
                         "var src = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue({ n: 2 }); c.close(); } });"
                         "var copy = structuredClone(src, { transfer: [src] });"
                         "R.proto = Object.getPrototypeOf(copy) === ReadableStream.prototype; R.locked = src.locked; R.copyLocked = copy.locked;"
                         "(async function () { var rd = copy.getReader(); for (;;) { var r = await rd.read(); if (r.done) { break; } R.chunks.push(JSON.stringify(r.value)); } R.done = true; })();"
                         "class Sub extends ReadableStream {} var sub = new Sub(); var subCopy = structuredClone(sub, { transfer: [sub] }); R.subProto = Object.getPrototypeOf(subCopy) === ReadableStream.prototype;"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "R.proto === true && R.locked === true && R.copyLocked === false && R.subProto === true"));
    EMB_CHECK(js_true(&e, "R.done === true && R.chunks.join('|') === '\"a\"|{\"n\":2}'"));
    EMB_CHECK(js_true(&e, "(function () { var s = new ReadableStream(); s.getReader(); try { structuredClone(s, { transfer: [s] }); } catch (x) { return x.name === 'DataCloneError'; } return false; })()"));
    msg_env_done(&e);
}

EMB_TEST(non_transferable_objects_in_the_transfer_list_fail)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_true(&e, "(function () { var b = new Blob(); try { structuredClone(b, { transfer: [b] }); } catch (x) { return x.name === 'DataCloneError'; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { var b = new ArrayBuffer(4); try { structuredClone(1, { transfer: [b, b] }); } catch (x) { return x.name === 'DataCloneError'; } return false; })()"));
    msg_env_done(&e);
}

EMB_TEST(transfer_table_duplicates_rollback_and_malformed_entries)
{
    env e;
    msg_env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { got: [] }; var ch = new MessageChannel(); ch.port2.onmessage = function (ev) { R.got.push(ev.data); };"
                         "var other = new MessageChannel(); var dup = ''; var rb = ''; var bad = ''; var rs = '';"
                         "try { structuredClone(1, { transfer: [other.port1, other.port1] }); } catch (x) { dup = x.name; }"
                         /* a later serialization failure rolls the whole table back: nothing is detached or locked */
                         "try { structuredClone({ p: other.port1, f: function () {} }, { transfer: [other.port1] }); } catch (x) { rb = x.name; }"
                         "var st = new ReadableStream();"
                         "try { structuredClone({ s: st, f: function () {} }, { transfer: [st] }); } catch (x) { rs = x.name; }"
                         "try { structuredClone(1, { transfer: [1] }); } catch (x) { bad = x.name; }"
                         "try { structuredClone(1, { transfer: [{}] }); } catch (x) { bad += '/' + x.name; }"
                         "try { structuredClone(1, { transfer: 5 }); } catch (x) { bad += '/' + x.name; }"
                         "other.port2.onmessage = function (ev) { R.got.push('o:' + ev.data); }; other.port1.postMessage('still-open');"
                         "R.stLocked = st.locked;"));
    settle_tasks(&e);
    EMB_CHECK(js_true(&e, "dup === 'DataCloneError' && rb === 'DataCloneError' && rs === 'DataCloneError'"));
    EMB_CHECK(js_true(&e, "R.stLocked === false && R.got.join() === 'o:still-open'"));
    EMB_CHECK(js_true(&e, "bad === 'TypeError/DataCloneError/TypeError'"));
    msg_env_done(&e);
}

[[maybe_unused]] static int (*const keep_shared_main)(void) = bind_loader_main;

static int msg_main(void)
{
    {
        env warm;
        msg_env_init(&warm);
        (void)js_run(&warm, "var w = new MessageChannel(); w.port1.onmessage = function () {}; w.port2.postMessage(1); window.postMessage(1, '*');"
                            "new MessageEvent('x'); var tr = new ReadableStream(); structuredClone(tr, { transfer: [tr] }); new WritableStream(); new Blob(['x']).text();");
        settle_tasks(&warm);
        msg_env_done(&warm);
    }
    size_t base = embi_mem_live();
    int rc = emb_test_run_all();
    size_t end = embi_mem_live();
    if (end != base) {
        fprintf(stderr, "LEAK: %llu bytes still live after the run (baseline %llu)\n", (unsigned long long)end,
                (unsigned long long)base);
        rc = 1;
    }
    return rc;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return msg_main();
}
