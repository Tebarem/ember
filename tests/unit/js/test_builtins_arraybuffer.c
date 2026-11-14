#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* ArrayBuffer and SharedArrayBuffer through script (ECMA-262 25.1, 25.2 and the resizable, transfer
 * and immutable extensions). */

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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "arraybuffer.js", 0);
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

EMB_TEST(arraybuffer_constructor_and_length)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "new ArrayBuffer(8).byteLength === 8"));
    EMB_CHECK(js_true(e, "new ArrayBuffer().byteLength === 0"));
    EMB_CHECK(js_true(e, "ArrayBuffer.length === 1 && ArrayBuffer.name === 'ArrayBuffer'"));
    EMB_CHECK(js_true(e, "new ArrayBuffer(2.9).byteLength === 2"));
    EMB_CHECK(js_true(e, "(function(){ try { ArrayBuffer(4); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new ArrayBuffer(-1); return false; } catch (x) { "
                         "return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new ArrayBuffer(1)) === "
                         "'[object ArrayBuffer]'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(arraybuffer_resizable)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8});"
                         "rab.resizable === true && rab.maxByteLength === 8 && rab.byteLength === 4"));
    EMB_CHECK(js_true(e, "rab.resize(6), rab.byteLength === 6"));
    EMB_CHECK(js_true(e, "(function(){ try { rab.resize(9); return false; } catch (x) { "
                         "return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "rab.resize(2), rab.byteLength === 2"));
    EMB_CHECK(js_true(e, "new ArrayBuffer(4).resizable === false"));
    EMB_CHECK(js_true(e, "(function(){ try { new ArrayBuffer(9, {maxByteLength: 8}); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(arraybuffer_transfer_and_detach)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var b = new ArrayBuffer(4); var c = b.transfer();"
                         "b.detached === true && b.byteLength === 0 && c.byteLength === 4 && "
                         "c.detached === false"));
    EMB_CHECK(js_true(e, "(function(){ try { b.slice(0); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var r = new ArrayBuffer(2, {maxByteLength: 4}).transfer(3);"
                         "r.resizable === true && r.byteLength === 3"));
    EMB_CHECK(js_true(e, "new ArrayBuffer(2, {maxByteLength: 4}).transferToFixedLength(3)"
                         ".resizable === false"));
    EMB_CHECK(js_true(e, "var im = new ArrayBuffer(4).transferToImmutable();"
                         "im.immutable === true && im.byteLength === 4"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(arraybuffer_slice_and_is_view)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var s = new Uint8Array([1,2,3,4,5]).buffer.slice(1, -1);"
                         "s.byteLength === 3 && new Uint8Array(s)[0] === 2 && new Uint8Array(s)[2] === 4"));
    EMB_CHECK(js_true(e, "new ArrayBuffer(8).slice(-3).byteLength === 3"));
    EMB_CHECK(js_true(e, "new ArrayBuffer(8).slice(5, 2).byteLength === 0"));
    EMB_CHECK(js_true(e, "ArrayBuffer.isView(new Uint8Array(1)) && ArrayBuffer.isView(new DataView(new ArrayBuffer(1)))"));
    EMB_CHECK(js_true(e, "!ArrayBuffer.isView(new ArrayBuffer(1))"));
    EMB_CHECK(js_true(e, "ArrayBuffer[Symbol.species] === ArrayBuffer"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(shared_arraybuffer_grow)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var sab = new SharedArrayBuffer(4, {maxByteLength: 8});"
                         "sab.growable === true && sab.maxByteLength === 8 && sab.byteLength === 4"));
    EMB_CHECK(js_true(e, "sab.grow(7), sab.byteLength === 7"));
    EMB_CHECK(js_true(e, "(function(){ try { sab.grow(3); return false; } catch (x) { "
                         "return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "new SharedArrayBuffer(4).growable === false"));
    EMB_CHECK(js_true(e, "new SharedArrayBuffer(4).slice(1, 3).byteLength === 2"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new SharedArrayBuffer(1)) === "
                         "'[object SharedArrayBuffer]'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(arraybuffer_immutable_and_slice_immutable)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var src = new ArrayBuffer(6);"
                         "var im2 = src.sliceToImmutable(1, 4);"
                         "im2.immutable === true && im2.byteLength === 3 && im2.detached === false"));
    EMB_CHECK(js_true(e, "src.immutable === false"));
    EMB_CHECK(js_true(e, "(function(){ try { im2.resize(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(buffer_getters_reject_wrong_receiver)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "(function(){ try { Object.getOwnPropertyDescriptor("
                         "ArrayBuffer.prototype, 'byteLength').get.call({}); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength')"
                         ".get.name === 'get byteLength'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* ArrayBuffer.prototype.slice rejects a species result that is immutable (the memcpy would write it). */
EMB_TEST(arraybuffer_slice_rejects_immutable_species_result)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var ab = new ArrayBuffer(4); ab.constructor = {[Symbol.species]: function(n) {"
                         " return new ArrayBuffer(n).transferToImmutable(); }};"
                         "(function(){ try { ab.slice(0); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* sliceToImmutable throws a RangeError when the source shrank below the end during coercion. */
EMB_TEST(slice_to_immutable_shrink)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var rb = new ArrayBuffer(8, {maxByteLength: 16});"
                         "(function(){ try { rb.sliceToImmutable(0, {valueOf(){ rb.resize(2); return 6; }}); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "var rb2 = new ArrayBuffer(8, {maxByteLength: 16}); new Uint8Array(rb2).fill(5);"
                         "var im = rb2.sliceToImmutable(2, {valueOf(){ rb2.resize(6); return 6; }});"
                         "im.immutable && im.byteLength === 4 && new Uint8Array(im)[3] === 5"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
