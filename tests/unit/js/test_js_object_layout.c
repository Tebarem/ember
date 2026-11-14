/* Object layout (J5j): the 96-byte object, the dense block with its
 * capacity word in front of the first element, and the rare state (dictionary, sparse elements,
 * child root) that is allocated on first need. Each case drives one storage transition, checks the
 * observable result against the generic semantics, and checks that the live bytes return to the
 * baseline once the runtime is freed. */
#include "es6_util.h"

#include "js/heap.h"
#include "js/interp.h"
#include "js/object.h"

#include <SDL3/SDL_log.h>
#include <string.h>

static_assert(sizeof(embi_js_object) == 96, "a plain object is 96 bytes (J5j)");
static_assert(EMBI_JS_OBJ_INLINE_SLOTS == 4, "the object diet keeps 4 inline slots");
static_assert(sizeof(embi_js_rare) <= 24, "the rare struct holds three pointers");

/* Live bytes after a create/free cycle: lazily built process state is in place first. */
static size_t warm_base(void)
{
    es6_env w = es6_env_new();
    es6_env_free(w);
    return embi_mem_live();
}

/* The object a global name is bound to, or nullptr. */
static embi_js_object *global_obj(es6_env e, const char *name)
{
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    return embi_js_is_object(v) ? embi_js_as_object(v) : nullptr;
}

/* Ordinary objects start with no rare state: a plain object is only its header, shape, slots and
 * inline slots, so it stays in the 96-byte class. */
EMB_TEST(js_object_layout_plain_has_no_rare)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var p = {a: 1, b: 2}; var q = []; var f = function () {}; true", true);
    embi_js_object *p = global_obj(e, "p");
    embi_js_object *q = global_obj(e, "q");
    EMB_REQUIRE(p != nullptr && q != nullptr);
    EMB_CHECK(p->rare == nullptr);
    EMB_CHECK(q->rare == nullptr);
    EMB_CHECK(embi_js_obj_dict(p) == nullptr);
    EMB_CHECK(embi_js_obj_sparse(q) == nullptr);
    EMB_CHECK(embi_js_obj_elems_len(q) == 0);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* More than 64 named properties turn the object into a dictionary, and a delete that is not the
 * last property does too. Key order and values survive both transitions. */
EMB_TEST(js_object_layout_dict_mode_and_back)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var o = {}; for (var i = 0; i < 70; i++) o['k' + i] = i; true", true);
    embi_js_object *o = global_obj(e, "o");
    EMB_REQUIRE(o != nullptr);
    EMB_CHECK(embi_js_obj_dict(o) != nullptr);
    E6_BOOL(e, "delete o.k3; o.k69 === 69 && o.k3 === undefined && Object.keys(o).length === 69",
            true);
    E6_BOOL(e, "Object.keys(o)[0] === 'k0' && Object.keys(o)[3] === 'k4'", true);
    E6_BOOL(e, "o.k3 = 'back'; Object.keys(o)[69] === 'k3' && o.k3 === 'back'", true);

    E6_BOOL(e, "var s = {a: 1, b: 2, c: 3}; delete s.a; s.b === 2 && Object.keys(s).join() === 'b,c'",
            true);
    embi_js_object *s = global_obj(e, "s");
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK(embi_js_obj_dict(s) != nullptr);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Sparse elements: a write far past the dense range moves the elements to the sparse dictionary,
 * and the length truncation removes the sparse keys above the new length. */
EMB_TEST(js_object_layout_sparse_elements_and_truncate)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var a = []; a[1000000] = 'far'; a[0] = 'near'; a.length === 1000001 && "
               "a[1000000] === 'far'",
            true);
    embi_js_object *a = global_obj(e, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(embi_js_obj_sparse(a) != nullptr);
    EMB_CHECK(embi_js_obj_elems_len(a) == 0);
    EMB_CHECK(!embi_js_obj_dense_plain(a));
    E6_BOOL(e, "Object.keys(a).join() === '0,1000000'", true);
    E6_BOOL(e, "a.length = 1; a.length === 1 && a[0] === 'near' && a[1000000] === undefined && "
               "Object.keys(a).join() === '0'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Dense growth: pushes reallocate the block (the capacity word tracks it), a delete leaves a hole
 * that clears PACKED, and a truncation keeps the surviving elements. */
EMB_TEST(js_object_layout_dense_growth_and_holes)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var b = []; for (var i = 0; i < 5000; i++) b.push(i * 2); "
               "b.length === 5000 && b[4999] === 9998",
            true);
    embi_js_object *b = global_obj(e, "b");
    EMB_REQUIRE(b != nullptr);
    EMB_CHECK(embi_js_obj_elems_len(b) == 5000);
    EMB_CHECK(embi_js_obj_is_packed(b));
    EMB_CHECK(embi_js_obj_dense_plain(b));
    E6_BOOL(e, "delete b[2]; b.length === 5000 && !(2 in b)", true);
    EMB_CHECK(!embi_js_obj_is_packed(b));
    E6_BOOL(e, "b.length = 3; b.length === 3 && b[1] === 2", true);
    E6_NUM(e, "var s = 0; for (var j = 0; j < 3; j++) s += b[j] === undefined ? 100 : b[j]; s",
           102);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* The C side of the block: reserve keeps the length, and the pushes that follow fill it in order. */
EMB_TEST(js_object_layout_reserve_and_push)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    embi_js_object *a = embi_js_array_create_default(e.rt, 0);
    EMB_REQUIRE(a != nullptr);
    embi_js_array_reserve(e.rt, a, 100);
    EMB_CHECK(embi_js_obj_elems_len(a) == 0);
    EMB_CHECK(embi_js_obj_is_packed(a));
    for (int i = 0; i < 100; i++) {
        EMB_CHECK(embi_js_array_push_fast(e.rt, a, embi_js_from_int32(i)));
    }
    EMB_CHECK(embi_js_obj_elems_len(a) == 100);
    EMB_CHECK(embi_js_as_int32(embi_js_obj_elems(a)[0]) == 0);
    EMB_CHECK(embi_js_as_int32(embi_js_obj_elems(a)[99]) == 99);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A prototype gets a cached child root on the first child, and the children keep finding its
 * properties through it. A dictionary prototype works the same way. */
EMB_TEST(js_object_layout_prototype_children)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function P() {} P.prototype.x = 1; var c1 = new P(), c2 = new P(); "
               "c1.x === 1 && c2.x === 1 && c1.__proto__ === P.prototype",
            true);
    embi_js_object *pr = global_obj(e, "P");
    EMB_REQUIRE(pr != nullptr);
    E6_BOOL(e, "var q = {y: 2}; var c3 = Object.create(q); q.y = 5; c3.y === 5 && "
               "(Object.setPrototypeOf(c3, {y: 9}), c3.y === 9)",
            true);
    E6_BOOL(e, "var dp = {}; for (var i = 0; i < 70; i++) dp['p' + i] = i; "
               "var kid = Object.create(dp); kid.p69 === 69 && dp.p69 === 69",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Rare state and dense blocks survive a full collection while only the JS heap roots them. */
EMB_TEST(js_object_layout_rare_survives_gc)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var keep = []; for (var i = 0; i < 3000; i++) { var o = {}; "
               "for (var j = 0; j < 70; j++) o['k' + j] = i + j; if (i % 300 === 0) keep.push(o); } "
               "keep.length === 10",
            true);
    E6_BOOL(e, "var sp = []; sp[500000] = 'x'; var dn = []; for (var m = 0; m < 3000; m++) dn.push(m); "
               "true",
            true);
    embi_js_gc(e.rt);
    E6_BOOL(e, "var ok = true; for (var n = 0; n < keep.length; n++) { var t = keep[n]; "
               "ok = ok && t.k69 === n * 300 + 69 && Object.keys(t).length === 70; } ok",
            true);
    E6_BOOL(e, "sp[500000] === 'x' && sp.length === 500001 && dn[2999] === 2999 && dn.length === 3000",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
