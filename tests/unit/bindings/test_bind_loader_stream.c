/*
 * ReadableStream through the bindings (W7c): Response.body and Blob.stream readers, locking and disturbance, async
 * iteration, the default controller with start, pull and cancel callbacks, queueing strategy and backpressure, errors, and
 * stream-backed bodies read through Response.
 */
#include "loader_env.h"

EMB_TEST(response_body_reader_reads_chunks_then_done)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var resp = new Response('streamed'); R.hasBody = resp.body instanceof ReadableStream; var rd = resp.body.getReader();"
                         "R.locked = resp.body.locked; R.same = resp.body === resp.body;"
                         "rd.read().then(function (r) { R.first = r.done + ':' + (r.value instanceof Uint8Array) + ':' + r.value.length; return rd.read(); })"
                         ".then(function (r) { R.second = r.done + ':' + r.value; return rd.closed; }).then(function () { R.closed = true; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.hasBody && R.locked === true && R.same && R.first === 'false:true:8' && R.second === 'true:undefined' && R.closed === true"));
    EMB_CHECK(js_true(&e, "new Response(null).body === null && new Response('').body instanceof ReadableStream"));
    env_done(&e);
}

EMB_TEST(stream_lock_and_disturb_rules)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "var resp = new Response('abc'); var s = resp.body; var r1 = s.getReader();"
                          "(function () { try { s.getReader(); return false; } catch (err) { return err instanceof TypeError; } })() && resp.bodyUsed === false"));
    EMB_CHECK(js_run(&e, "var R = {}; resp.text().catch(function (err) { R.locked = err instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.locked === true"));
    EMB_CHECK(js_run(&e, "r1.releaseLock(); r1.read().catch(function (err) { R.released = err instanceof TypeError; }); s.getReader().read().then(function (c) { R.chunk = c.value.length; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.released === true && R.chunk === 3 && resp.bodyUsed === true"));
    env_done(&e);
}

EMB_TEST(stream_async_iteration)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { sizes: [] };"
                         "(async function () { var total = 0; for await (const chunk of new Response('iterate me').body) { total += chunk.length; R.sizes.push(chunk.length); } R.total = total; })()"
                         ".catch(function (err) { R.err = String(err); });"
                         "(async function () { var s = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } }); var out = '';"
                         "  for await (const x of s) { out += x; } R.out = out; })().catch(function (err) { R.err2 = String(err); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.err2 === undefined && R.total === 10 && R.out === 'ab'"));
    env_done(&e);
}

EMB_TEST(stream_controller_start_pull_cancel)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { log: [] }; var n = 0;"
                         "var s = new ReadableStream({"
                         "  start(c) { R.log.push('start'); },"
                         "  pull(c) { R.log.push('pull' + n); if (n < 3) { c.enqueue('chunk' + (n++)); } else { c.close(); } },"
                         "  cancel(reason) { R.log.push('cancel:' + reason); }"
                         "}, { highWaterMark: 1 });"
                         "var rd = s.getReader(); var got = [];"
                         "function next() { return rd.read().then(function (r) { if (r.done) { R.got = got.join(); return; } got.push(r.value); return next(); }); }"
                         "next().catch(function (err) { R.err = String(err); });"
                         "var c2 = new ReadableStream({ cancel(reason) { R.cancelled = reason; } }); c2.cancel('bye').then(function () { R.cancelDone = true; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.got === 'chunk0,chunk1,chunk2' && R.log[0] === 'start' && R.cancelled === 'bye' && R.cancelDone === true"));
    env_done(&e);
}

EMB_TEST(stream_errors_and_strategy)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var s = new ReadableStream({ start(c) { c.error(new RangeError('boom')); } });"
                         "s.getReader().read().catch(function (err) { R.errName = err.name; });"
                         "try { new ReadableStream({ start(c) { throw new TypeError('in start'); } }); } catch (err) { R.startErr = err.message; }"
                         "var u = new ReadableStream({ start(c) { R.ds0 = c.desiredSize; c.enqueue('x'); R.ds1 = c.desiredSize; c.close(); R.ds2 = c.desiredSize; } }, { highWaterMark: 3 });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.errName === 'RangeError' && R.startErr === 'in start' && R.ds0 === 3 && R.ds1 === 2 && R.ds2 === 2"));
    EMB_CHECK(js_true(&e, "(function () { try { new ReadableStream({ start(c) { c.close(); c.enqueue('late'); } }); return false; } catch (err) { return err instanceof TypeError; } })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new ReadableStream({ type: 'bytes' }); return true; } catch (err) { return err instanceof TypeError || err instanceof RangeError; } })()"));
    env_done(&e);
}

EMB_TEST(stream_backed_response_bodies)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var enc = new TextEncoder();"
                         "var s = new ReadableStream({ start(c) { c.enqueue(enc.encode('part1,')); c.enqueue(enc.encode('part2')); c.close(); } });"
                         "new Response(s).text().then(function (t) { R.t = t; });"
                         "var bad = new ReadableStream({ start(c) { c.enqueue('not bytes'); c.close(); } });"
                         "new Response(bad).text().catch(function (err) { R.bad = err instanceof TypeError; });"
                         "new Blob(['from blob']).stream().getReader().read().then(function (r) { R.blobChunk = r.value.length; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.t === 'part1,part2' && R.bad === true && R.blobChunk === 9"));
    EMB_CHECK(js_run(&e, "var used = new ReadableStream({ start(c) { c.enqueue(new Uint8Array(1)); c.close(); } }); var rr = new Response(used);"
                         "used.getReader().read().then(function () { R.afterRead = 1; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.afterRead === 1"));
    env_done(&e);
}

BIND_LOADER_MAIN()
