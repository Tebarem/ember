#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* DataView and Atomics through script (ECMA-262 25.3 and 25.4, Atomics.pause). */

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "dataview.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

EMB_TEST(dataview_endianness_and_widths)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var dv = new DataView(new ArrayBuffer(8));"
                         "dv.setUint16(0, 0x1234); dv.getUint8(0) === 0x12 && dv.getUint8(1) === 0x34"));
    EMB_CHECK(js_true(e, "dv.setUint16(0, 0x1234, true); dv.getUint8(0) === 0x34 && dv.getUint16(0) === 0x3412"));
    EMB_CHECK(js_true(e, "dv.setInt32(4, -2); dv.getInt32(4) === -2 && dv.getUint32(4) === 4294967294"));
    EMB_CHECK(js_true(e, "dv.setFloat32(0, 1.5, true); dv.getFloat32(0, true) === 1.5"));
    EMB_CHECK(js_true(e, "dv.setFloat64(0, Math.PI); dv.getFloat64(0) === Math.PI"));
    EMB_CHECK(js_true(e, "dv.setFloat16(0, 65504); dv.getFloat16(0) === 65504 && "
                         "dv.getFloat16(0, true) !== 65504"));
    EMB_CHECK(js_true(e, "dv.setBigInt64(0, -1n); dv.getBigUint64(0) === 2n ** 64n - 1n && "
                         "dv.getBigInt64(0, true) === -1n"));
    EMB_CHECK(js_true(e, "dv.setInt8(7, -1); dv.getUint8(7) === 255"));
    EMB_CHECK(js_true(e, "dv.setInt8(0, 200), dv.getInt8(0) === -56"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(DataView.prototype, 'getInt8').value.length === 1 && "
                         "DataView.prototype.setInt8.length === 2"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(dataview_bounds_and_receivers)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "(function(){ try { new DataView(new ArrayBuffer(4)).getInt32(1); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new DataView(new ArrayBuffer(4), 5); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { DataView(new ArrayBuffer(4)); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new DataView({}); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { DataView.prototype.getInt8.call({}, 0); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var d = new DataView(new ArrayBuffer(8), 2, 4);"
                         "d.byteOffset === 2 && d.byteLength === 4 && d.buffer.byteLength === 8"));
    EMB_CHECK(js_true(e, "new DataView(new ArrayBuffer(4)).getInt8(3) === 0 && "
                         "(function(){ try { new DataView(new ArrayBuffer(4)).getInt8(-1); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(dataview_length_tracking_and_detach)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8});"
                         "var dt = new DataView(rab, 1);"
                         "dt.byteLength === 3 && rab.resize(8), dt.byteLength === 7"));
    EMB_CHECK(js_true(e, "rab.resize(0); dt.byteLength === 0 && dt.byteOffset === 0"));
    EMB_CHECK(js_true(e, "var det = new DataView(new ArrayBuffer(4)); det.buffer.transfer();"
                         "(function(){ try { det.getInt8(0); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(atomics_read_modify_write)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var ta = new Int32Array(new SharedArrayBuffer(16));"
                         "Atomics.add(ta, 0, 5) === 0 && Atomics.load(ta, 0) === 5"));
    EMB_CHECK(js_true(e, "Atomics.sub(ta, 0, 2) === 5 && ta[0] === 3 && Atomics.and(ta, 0, 1) === 3"));
    EMB_CHECK(js_true(e, "Atomics.or(ta, 0, 6) === 1 && Atomics.xor(ta, 0, 1) === 7 && ta[0] === 6"));
    EMB_CHECK(js_true(e, "Atomics.exchange(ta, 0, 9) === 6 && ta[0] === 9"));
    EMB_CHECK(js_true(e, "Atomics.compareExchange(ta, 0, 9, 1) === 9 && ta[0] === 1"));
    EMB_CHECK(js_true(e, "Atomics.compareExchange(ta, 0, 2, 3) === 1 && ta[0] === 1"));
    EMB_CHECK(js_true(e, "Atomics.store(ta, 1, -0) === 0 && Object.is(Atomics.store(ta, 1, -0), 0)"));
    EMB_CHECK(js_true(e, "Atomics.store(ta, 1, 3.7) === 3 && ta[1] === 3"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.load(ta, 4); return false; } catch (x) { "
                         "return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.load(new Float64Array(1), 0); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.add(new Uint8ClampedArray(1), 0, 1); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var u8 = new Uint8Array(2); Atomics.add(u8, 0, 300) === 0 && u8[0] === 44"));
    EMB_CHECK(js_true(e, "var bi = new BigInt64Array(new SharedArrayBuffer(8));"
                         "Atomics.add(bi, 0, 5n) === 0n && Atomics.load(bi, 0) === 5n && "
                         "Atomics.sub(bi, 0, 1n) === 5n"));
    EMB_CHECK(js_true(e, "Atomics[Symbol.toStringTag] === 'Atomics' && Atomics.add.length === 3"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(atomics_wait_notify_and_pause)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var sh = new Int32Array(new SharedArrayBuffer(8));"
                         "var thr = function(f){ try { f(); return false; } catch (x) { return x instanceof TypeError; } };"
                         "thr(function(){ Atomics.wait(sh, 0, 1, 0); }) && thr(function(){ Atomics.wait(sh, 0, 0, 10); })"
                         " && thr(function(){ Atomics.wait(sh, 0, 0, 1e9); }) && typeof Atomics.waitAsync === 'undefined'"));
    /* The coercions run before the TypeError. */
    EMB_CHECK(js_true(e, "var hit = 0; thr(function(){ Atomics.wait(sh, 0, {valueOf(){ hit++; return 0; }}, {valueOf(){ hit++; return 0; }}); }) && hit === 2"));
    EMB_CHECK(js_true(e, "Atomics.notify(sh, 0) === 0 && Atomics.notify(sh, 0, 3) === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.wait(new Int32Array(4), 0, 0, 0); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.wait(sh, 0, 0); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "Atomics.notify(new Int32Array(4), 0) === 0"));
    EMB_CHECK(js_true(e, "Atomics.isLockFree(4) === true && Atomics.isLockFree(3) === false"));
    EMB_CHECK(js_true(e, "Atomics.pause() === undefined && Atomics.pause(3) === undefined"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.pause(1.5); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Every DataView setter throws on an immutable buffer before reading its arguments. */
EMB_TEST(dataview_immutable_buffer_set_rejects_before_args)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var dvi = new DataView(new ArrayBuffer(8).transferToImmutable());"
                         "var reads = 0; var probe = {valueOf() { reads++; return 0; }};"
                         "var names = ['setInt8','setUint8','setInt16','setUint16','setInt32','setUint32',"
                         "'setFloat16','setFloat32','setFloat64','setBigInt64','setBigUint64'];"
                         "names.every(function(n){ try { dvi[n](probe, probe, probe); return false; }"
                         " catch (x) { return x instanceof TypeError; } }) && reads === 0"));
    EMB_CHECK(js_true(e, "dvi.getUint8(0) === 0 && dvi.buffer.immutable === true"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A DataView byteLength reads 0 once its buffer shrinks under it, and so does byteOffset. */
EMB_TEST(dataview_out_of_bounds_byte_length_is_zero)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var ab = new ArrayBuffer(4, {maxByteLength: 5}); var dv = new DataView(ab, 1, 2);"
                         "dv.byteLength === 2 && (ab.resize(2), dv.byteLength === 0)"));
    EMB_CHECK(js_true(e, "dv.byteOffset === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { dv.getUint8(0); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
