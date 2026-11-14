/* Protectors and inline caches (J5e). A store site that has cached
 * an ordinary object must not clear or skip a protector when it later stores onto a watched intrinsic
 * whose shape it shares. Each case warms one site on a shape-identical clone, then stores onto the
 * intrinsic through the same site, and checks the protector bit and the observable behavior. */
#include "es6_util.h"

#include "js/interp.h"
#include "js/object.h"

#include <SDL3/SDL_log.h>
#include <string.h>

static size_t warm_base(void)
{
    es6_env w = es6_env_new();
    es6_env_free(w);
    return embi_mem_live();
}

static uint32_t protect_bits(es6_env e)
{
    return e.rt->protect;
}

/* A store through a cached site onto %ArrayIteratorPrototype%.next clears ARRAY_ITERATOR, so for-of
 * calls the user's next (the for-of fast path is off) and the body never runs. */
EMB_TEST(js_array_fast_ic_watched_next_store)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var AIP = Object.getPrototypeOf([][Symbol.iterator]());"
            "var clone = Object.create(Object.getPrototypeOf(AIP));"
            "Object.defineProperty(clone, 'next', { value: 0, writable: true, enumerable: false, configurable: true });"
            "Object.defineProperty(clone, Symbol.toStringTag, { value: 'Array Iterator', writable: false,"
            "  enumerable: false, configurable: true });"
            "function set(o, f) { o.next = f; }"
            "set(clone, 1); set(clone, 2); set(clone, 3); true",
            true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_ITERATOR);
    E6_BOOL(e,
            "var origNext = AIP.next; var calls = 0;"
            "set(AIP, function () { calls++; return { done: true, value: undefined }; });"
            "var body = 0; for (var y of [1, 2]) body++;"
            "var ok = body === 0 && calls === 1; AIP.next = origNext; ok",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_ITERATOR));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* The same through the constructor: a cached store of Array.prototype.constructor clears
 * ARRAY_SPECIES, so map uses the replaced constructor's species. */
EMB_TEST(js_array_fast_ic_watched_constructor_store)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var cc = Object.create(Object.prototype);"
            "Object.defineProperty(cc, 'constructor', { value: 0, writable: true, enumerable: false, configurable: true });"
            "function setc(o, f) { o.constructor = f; }"
            "setc(cc, 1); setc(cc, 2); setc(cc, 3); true",
            true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_SPECIES);
    E6_BOOL(e,
            "var origC = Array.prototype.constructor; var seenSpecies = 0;"
            "function Fake(n) { seenSpecies++; } Fake[Symbol.species] = Fake;"
            "setc(Array.prototype, Fake);"
            "var mm = [1, 2].map(function (v) { return v; });"
            "var ok = seenSpecies === 1 && mm instanceof Fake; Array.prototype.constructor = origC; ok",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_SPECIES));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A cached store on an ordinary object does not clear protectors (no regression for the common case),
 * so the for-of fast path keeps applying to plain arrays. */
EMB_TEST(js_array_fast_ic_ordinary_store_keeps_protectors)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function setn(o, f) { o.next = f; }"
            "var plain = {}; setn(plain, 1); setn(plain, 2); var sum = 0;"
            "for (var z of [4, 5]) sum += z; sum === 9",
            true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_ITERATOR);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
