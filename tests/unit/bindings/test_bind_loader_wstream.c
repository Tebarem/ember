/*
 * WritableStream, TransformStream, pipeTo, pipeThrough, TextEncoderStream and TextDecoderStream (W7f): the JS-written
 * part of the Streams API (src/bindings/js/streams.js) on top of the native ReadableStream.
 */
#include "loader_env.h"

EMB_TEST(lazy_globals_become_data_properties)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "typeof WritableStream === 'function' && typeof TransformStream === 'function' && typeof TextEncoderStream === 'function'"));
    EMB_CHECK(js_true(&e, "(function () { var d = Object.getOwnPropertyDescriptor(globalThis, 'WritableStream'); return 'value' in d && d.writable && d.configurable && !d.enumerable; })()"));
    EMB_CHECK(js_true(&e, "typeof WritableStreamDefaultWriter === 'function' && typeof WritableStreamDefaultController === 'function' && typeof TransformStreamDefaultController === 'function' && typeof TextDecoderStream === 'function'"));
    EMB_CHECK(js_true(&e, "globalThis.TextDecoderStream = 5; TextDecoderStream === 5"));
    EMB_CHECK(js_true(&e, "typeof ReadableStream.prototype.pipeTo === 'function' && typeof ReadableStream.prototype.pipeThrough === 'function'"));
    env_done(&e);
}

EMB_TEST(writable_stream_writes_in_order_and_closes)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { log: [] };"
                         "var ws = new WritableStream({ start(c) { R.log.push('start'); R.hasSignal = c.signal instanceof AbortSignal; },"
                         "  write(chunk, c) { R.log.push('write:' + chunk); return Promise.resolve(); },"
                         "  close() { R.log.push('close'); } }, { highWaterMark: 2 });"
                         "var w = ws.getWriter(); R.locked = ws.locked; R.ds0 = w.desiredSize;"
                         "w.write('a'); w.write('b'); R.ds1 = w.desiredSize;"
                         "w.write('c').then(function () { return w.close(); }).then(function () { R.closed = true; R.log.push('done'); });"
                         "w.closed.then(function () { R.closedP = true; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.locked === true && R.ds0 === 2 && R.ds1 === 0 && R.hasSignal === true"));
    EMB_CHECK(js_true(&e, "R.log.slice(0, 2).join() === 'start,write:a'"));
    env_done(&e);
}

EMB_TEST(writable_stream_state_machine)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { log: [] };"
                         "var ws = new WritableStream({ write(chunk) { R.log.push(chunk); }, close() { R.log.push('close'); } });"
                         "var w = ws.getWriter();"
                         "(async function () { await w.ready; await w.write(1); await w.write(2); await w.close(); R.after = w.desiredSize; R.log.push('end');"
                         "  try { await w.write(3); } catch (x) { R.writeAfterClose = x instanceof TypeError; }"
                         "  w.releaseLock(); R.unlocked = !ws.locked;"
                         "  try { await w.close(); } catch (x) { R.releasedClose = x instanceof TypeError; } })().catch(function (x) { R.err = String(x); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.log.join() === '1,2,close,end' && R.after === 0 && R.writeAfterClose === true && R.unlocked === true && R.releasedClose === true"));
    EMB_CHECK(js_run(&e, "var R2 = {}; var ws2 = new WritableStream({ write() { throw new RangeError('bad'); } }); var w2 = ws2.getWriter();"
                         "w2.write(1).catch(function (x) { R2.w = x.name; }); w2.closed.catch(function (x) { R2.c = x.name; }); w2.ready.catch(function (x) { R2.r = x.name; });"
                         "var ws3 = new WritableStream({ abort(reason) { R2.reason = reason; } }); ws3.abort('stop').then(function () { R2.aborted = true; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R2.w === 'RangeError' && R2.c === 'RangeError' && R2.r === 'RangeError' && R2.reason === 'stop' && R2.aborted === true"));
    env_done(&e);
}

EMB_TEST(writable_stream_argument_validation)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_true(&e, "(function () { try { new WritableStream({ type: 'bytes' }); } catch (x) { return x instanceof RangeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new WritableStream({}, { highWaterMark: -1 }); } catch (x) { return x instanceof RangeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new WritableStream({ write: 5 }); } catch (x) { return x instanceof TypeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { WritableStream.prototype.getWriter.call({}); } catch (x) { return x instanceof TypeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new WritableStreamDefaultController(); } catch (x) { return x instanceof TypeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { var ws = new WritableStream(); ws.getWriter(); try { ws.getWriter(); } catch (x) { return x instanceof TypeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "Object.prototype.toString.call(new WritableStream()) === '[object WritableStream]' && new WritableStream({}, { size() { return 1; } }) instanceof WritableStream"));
    env_done(&e);
}

EMB_TEST(transform_stream_transforms_and_flushes)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { out: [] };"
                         "var ts = new TransformStream({ start(c) { R.ds = c.desiredSize; }, transform(chunk, c) { c.enqueue(chunk.toUpperCase()); c.enqueue(chunk + '!'); },"
                         "  flush(c) { c.enqueue('end'); } });"
                         "var w = ts.writable.getWriter(); var r = ts.readable.getReader();"
                         "(async function () { w.write('a'); w.write('b'); w.close();"
                         "  for (;;) { var x = await r.read(); if (x.done) { break; } R.out.push(x.value); } R.done = true; })().catch(function (x) { R.err = String(x); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.done === true && R.out.join() === 'A,a!,B,b!,end' && R.ds === 0"));
    EMB_CHECK(js_true(&e, "Object.prototype.toString.call(new TransformStream()) === '[object TransformStream]' && new TransformStream().readable instanceof ReadableStream && new TransformStream().writable instanceof WritableStream"));
    env_done(&e);
}

EMB_TEST(transform_stream_identity_backpressure_and_errors)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var id = new TransformStream(); var w = id.writable.getWriter(); var r = id.readable.getReader();"
                         "R.ready0 = w.desiredSize; w.write('x').then(function () { R.wrote = true; }); r.read().then(function (v) { R.val = v.value; });"
                         "var bad = new TransformStream({ transform() { throw new TypeError('nope'); } }); var bw = bad.writable.getWriter();"
                         "bw.write(1).catch(function (x) { R.badW = x.message; }); bad.readable.getReader().read().catch(function (x) { R.badR = x.message; });"
                         "var term = new TransformStream({ start(c) { c.terminate(); } }); term.readable.getReader().read().then(function (v) { R.term = v.done; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.ready0 === 1 && R.wrote === true && R.val === 'x' && R.badW === 'nope' && R.badR === 'nope' && R.term === true"));
    EMB_CHECK(js_true(&e, "(function () { try { new TransformStream({ readableType: 'bytes' }); } catch (x) { return x instanceof RangeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new TransformStreamDefaultController(); } catch (x) { return x instanceof TypeError; } return false; })()"));
    env_done(&e);
}

EMB_TEST(pipeTo_copies_chunks_and_closes)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { got: [] };"
                         "var rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.enqueue(3); c.close(); } });"
                         "var ws = new WritableStream({ write(x) { R.got.push(x); }, close() { R.closed = true; } });"
                         "var p = rs.pipeTo(ws); R.lockedNow = rs.locked && ws.locked;"
                         "p.then(function (v) { R.resolved = v === undefined; R.unlocked = !rs.locked && !ws.locked; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.got.join() === '1,2,3' && R.closed === true && R.resolved === true && R.unlocked === true && R.lockedNow === true"));
    env_done(&e);
}

EMB_TEST(pipeTo_propagates_errors_and_honours_options)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {};"
                         "var rs = new ReadableStream({ start(c) { c.error('src failed'); } });"
                         "var ws = new WritableStream({ abort(r) { R.abortReason = r; } });"
                         "rs.pipeTo(ws).catch(function (x) { R.e1 = x; });"
                         "var rs2 = new ReadableStream({ start(c) { c.enqueue(1); } , cancel(r) { R.cancelReason = r; } });"
                         "var ws2 = new WritableStream({ write() { throw 'sink failed'; } });"
                         "rs2.pipeTo(ws2).catch(function (x) { R.e2 = x; });"
                         "var rs3 = new ReadableStream({ start(c) { c.close(); } }); var closed3 = false; var ws3 = new WritableStream({ close() { closed3 = true; } });"
                         "rs3.pipeTo(ws3, { preventClose: true }).then(function () { R.pc = closed3 === false && !ws3.locked; });"
                         "var rs4 = new ReadableStream({ start(c) { c.error('e4'); } }); var ws4 = new WritableStream({ abort() { R.abort4 = true; } });"
                         "rs4.pipeTo(ws4, { preventAbort: true }).catch(function (x) { R.pa = x === 'e4' && R.abort4 !== true; });"
                         "var rs5 = new ReadableStream({ pull() {}, cancel() { R.cancel5 = true; } }); var ac = new AbortController(); var ws5 = new WritableStream();"
                         "rs5.pipeTo(ws5, { signal: ac.signal }).catch(function (x) { R.sig = x.name; }); ac.abort();"
                         "rs.pipeTo(5).catch(function (x) { R.badDest = x instanceof TypeError; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.e1 === 'src failed' && R.abortReason === 'src failed' && R.e2 === 'sink failed' && R.cancelReason === 'sink failed'"));
    EMB_CHECK(js_true(&e, "R.pc === true && R.pa === true && R.sig === 'AbortError' && R.cancel5 === true && R.badDest === true"));
    env_done(&e);
}

EMB_TEST(pipeTo_disturbs_the_body_synchronously)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = {}; var resp = new Response('hello'); var ws = new WritableStream({ write(c) { R.n = c.length; } });"
                         "var p = resp.body.pipeTo(ws); R.used = resp.bodyUsed;"
                         "var r2 = new Response('x'); var t = new TransformStream(); var out = r2.body.pipeThrough(t); R.used2 = r2.bodyUsed; R.same = out === t.readable;"
                         "p.then(function () { R.piped = true; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.used === true && R.used2 === true && R.same === true && R.piped === true && R.n === 5"));
    env_done(&e);
}

EMB_TEST(pipeThrough_chains_and_validates)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { out: [] };"
                         "var rs = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });"
                         "var up = new TransformStream({ transform(x, c) { c.enqueue(x + x); } });"
                         "var rd = rs.pipeThrough(up).getReader();"
                         "(async function () { for (;;) { var x = await rd.read(); if (x.done) { break; } R.out.push(x.value); } R.done = true; })();"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.done === true && R.out.join() === 'aa,bb'"));
    EMB_CHECK(js_true(&e, "(function () { var s = new ReadableStream(); s.getReader(); try { s.pipeThrough(new TransformStream()); } catch (x) { return x instanceof TypeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "(function () { try { new ReadableStream().pipeThrough({ readable: {}, writable: {} }); } catch (x) { return x instanceof TypeError; } return false; })()"));
    env_done(&e);
}

EMB_TEST(text_encoder_stream_encodes_and_pairs_surrogates)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { out: [] }; var tes = new TextEncoderStream(); R.enc = tes.encoding; R.rb = tes.readable instanceof ReadableStream && tes.writable instanceof WritableStream;"
                         "var w = tes.writable.getWriter(); var r = tes.readable.getReader();"
                         "(async function () { w.write('h\\u00e9'); w.write('\\ud83d'); w.write('\\ude00x'); w.write('\\ud83d'); w.close();"
                         "  for (;;) { var x = await r.read(); if (x.done) { break; } R.out.push(Array.from(x.value).join('.')); } R.done = true; })().catch(function (x) { R.err = String(x); });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.enc === 'utf-8' && R.rb === true && R.done === true"));
    /* h é | (held) 😀 x | lone high at end -> EF BF BD */
    EMB_CHECK(js_true(&e, "R.out.join('|') === '104.195.169|240.159.152.128.120|239.191.189'"));
    EMB_CHECK(js_true(&e, "Object.prototype.toString.call(new TextEncoderStream()) === '[object TextEncoderStream]'"));
    env_done(&e);
}

EMB_TEST(text_decoder_stream_decodes_split_input)
{
    env e;
    env_init(&e);
    EMB_CHECK(js_run(&e, "var R = { out: '' }; var tds = new TextDecoderStream('utf-8', { ignoreBOM: true }); R.attrs = tds.encoding + tds.fatal + tds.ignoreBOM;"
                         "var w = tds.writable.getWriter(); var r = tds.readable.getReader(); var bytes = new TextEncoder().encode('a\\u00e9\\ud83d\\ude00z');"
                         "(async function () { for (var i = 0; i < bytes.length; i++) { w.write(bytes.subarray(i, i + 1)); } w.close();"
                         "  for (;;) { var x = await r.read(); if (x.done) { break; } R.out += x.value; } R.done = true; })().catch(function (x) { R.err = String(x); });"
                         "var f = new TextDecoderStream('utf-8', { fatal: true }); var fw = f.writable.getWriter();"
                         "fw.write(new Uint8Array([0xff])).catch(function (x) { R.fatal = x instanceof TypeError; }); f.readable.getReader().read().catch(function () {});"
                         "var b = new TextDecoderStream(); var bw = b.writable.getWriter(); bw.write('not a buffer').catch(function (x) { R.badChunk = x instanceof TypeError; }); b.readable.getReader().read().catch(function () {});"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.err === undefined && R.done === true && R.out === 'a\\u00e9\\ud83d\\ude00z' && R.attrs === 'utf-8falsetrue' && R.fatal === true && R.badChunk === true"));
    EMB_CHECK(js_true(&e, "(function () { try { new TextDecoderStream('no-such-label'); } catch (x) { return x instanceof RangeError; } return false; })()"));
    EMB_CHECK(js_true(&e, "new TextDecoderStream('utf-16le').encoding === 'utf-16le'"));
    env_done(&e);
}

EMB_TEST(empty_formdata_is_the_closing_delimiter_and_parses_back)
{
    env e;
    env_init(&e);
    /* The Fetch standard's multipart/form-data encoding of no entries is the closing delimiter alone. WPT's
     * response-form-data "Empty form data" needs exactly that; request-consume-empty/response-consume-empty expect "" for
     * the same input and cannot pass at the same time (tests/wpt-th/README.md). */
    EMB_CHECK(js_run(&e, "var R = {}; new Response(new FormData()).text().then(function (t) { R.t = t; R.ok = t.indexOf('--') === 0 && t.slice(-4) === '--' + String.fromCharCode(13, 10); });"
                         "new Response(new FormData()).formData().then(function (fd) { R.n = Array.from(fd.entries()).length; });"
                         "new Request('http://t.test/', { method: 'POST', body: new FormData() }).formData().then(function (fd) { R.req = fd instanceof FormData; });"
                         "var fd = new FormData(); fd.append('a', 'b'); new Response(fd).text().then(function (t) { R.full = t.length > 20; });"));
    settle(&e);
    EMB_CHECK(js_true(&e, "R.ok === true && R.n === 0 && R.req === true && R.full === true"));
    env_done(&e);
}

[[maybe_unused]] static int (*const keep_shared_main)(void) = bind_loader_main;

static int wstream_main(void)
{
    {
        env warm;
        env_init(&warm);
        (void)js_run(&warm, "var ws = new WritableStream({ write() {} }); var w = ws.getWriter(); w.write(1); w.close();"
                            "new ReadableStream({ start(c) { c.enqueue(1); c.close(); } }).pipeThrough(new TransformStream()); new TextEncoderStream(); new TextDecoderStream();"
                            "new Response(new FormData()).text();");
        settle(&warm);
        env_done(&warm);
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
    return wstream_main();
}
