/*
 * TreeWalker, NodeIterator and NodeFilter (W5f, src/bindings/impl_traversal.c): navigation, whatToShow and filters, the
 * pre-removing steps of NodeIterator (also while a filter runs), and the release of the iterators with the context.
 */
#include "emb_test.h"

#include "bindings/bind.h"
#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
} env;

static env env_new(void)
{
    env e = {0};
    e.rt = embi_js_runtime_new();
    e.doc = embi_document_new(true);
    embi_element *html = nullptr;
    embi_element *body = nullptr;
    (void)embi_dom_create_element(e.doc, EMBI_STR("html"), &html);
    (void)embi_append_child(&e.doc->base, &html->base);
    (void)embi_dom_create_element(e.doc, EMBI_STR("body"), &body);
    (void)embi_append_child(&html->base, &body->base);
    e.c = embi_bind_new(e.rt, e.doc);
    return e;
}

static void env_free(env e)
{
    embi_bind_free(e.c);
    embi_js_runtime_free(e.rt);
    embi_document_free(e.doc);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e.c), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = v == embi_js_from_bool(true);
    if (!ok) {
        SDL_Log("    js not true: %s", src);
    }
    return ok;
}

static const char k_tree[] = "document.body.innerHTML = '<div id=r><p id=a>t<b id=b></b></p><p id=c></p><i id=d></i></div>';"
                             "var r = document.getElementById('r'), a = document.getElementById('a'), b = document.getElementById('b'),"
                             "    c = document.getElementById('c'), d = document.getElementById('d'); true";

EMB_TEST(trav_tree_walker_navigation)
{
    env e = env_new();
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var w = document.createTreeWalker(r, NodeFilter.SHOW_ELEMENT);"
                         "w.root === r && w.currentNode === r && w.whatToShow === 1 && w.filter === null && "
                         "w.firstChild() === a && w.nextSibling() === c && w.nextSibling() === d && w.nextSibling() === null && "
                         "w.previousSibling() === c && w.parentNode() === r && w.parentNode() === null"));
    EMB_CHECK(js_true(e, "w.currentNode = r; var seq = []; var n; while ((n = w.nextNode())) seq.push(n.id); seq.join() === 'a,b,c,d' && "
                         "w.previousNode().id === 'c' && w.parentNode() === r"));
    EMB_CHECK(js_true(e, "var w2 = document.createTreeWalker(r, NodeFilter.SHOW_ELEMENT, {acceptNode: n => n.id === 'a' ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT});"
                         "var s2 = []; while ((n = w2.nextNode())) s2.push(n.id); s2.join() === 'c,d'"));
    EMB_CHECK(js_true(e, "var w3 = document.createTreeWalker(r, NodeFilter.SHOW_ELEMENT, n => n.id === 'a' ? NodeFilter.FILTER_SKIP : NodeFilter.FILTER_ACCEPT);"
                         "var s3 = []; while ((n = w3.nextNode())) s3.push(n.id); s3.join() === 'b,c,d'"));
    env_free(e);
}

EMB_TEST(trav_filter_errors_and_re_entry)
{
    env e = env_new();
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var thrown = false; var wt = document.createTreeWalker(r, 1, () => { throw new RangeError('x') });"
                         "try { wt.nextNode() } catch (x) { thrown = x instanceof RangeError } thrown"));
    EMB_CHECK(js_true(e, "var reentered = ''; var wr = document.createTreeWalker(r, 1, n => { try { wr.nextNode() } catch (x) { reentered = x.name } return 1 });"
                         "wr.nextNode(); reentered === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "var nof = false; try { document.createTreeWalker(r, 1, {}).nextNode() } catch (x) { nof = x instanceof TypeError } nof"));
    EMB_CHECK(js_true(e, "var wc = false; try { document.createNodeIterator(r, 1, 5) } catch (x) { wc = x instanceof TypeError } wc"));
    env_free(e);
}

EMB_TEST(trav_node_iterator_basics)
{
    env e = env_new();
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var it = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT);"
                         "it.root === r && it.referenceNode === r && it.pointerBeforeReferenceNode === true && "
                         "it.nextNode() === r && it.nextNode() === a && it.referenceNode === a && it.pointerBeforeReferenceNode === false && "
                         "it.previousNode() === a && it.pointerBeforeReferenceNode === true"));
    EMB_CHECK(js_true(e, "var all = document.createNodeIterator(r); var kinds = []; var n; while ((n = all.nextNode())) kinds.push(n.nodeType);"
                         "kinds.join() === '1,1,3,1,1,1'"));
    env_free(e);
}

EMB_TEST(trav_node_iterator_pre_removing_steps)
{
    env e = env_new();
    EMB_CHECK(js_true(e, k_tree));
    /* the reference node is removed: it moves to the previous node (pointer after) or the next one (pointer before) */
    EMB_CHECK(js_true(e, "var it = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT); it.nextNode(); it.nextNode(); it.nextNode();"
                         "it.referenceNode === b && it.pointerBeforeReferenceNode === false;"
                         "b.remove(); it.referenceNode.nodeType === 3 && it.pointerBeforeReferenceNode === false && it.nextNode() === c"));
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var it2 = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT); it2.nextNode(); it2.nextNode(); it2.previousNode();"
                         "it2.referenceNode === a && it2.pointerBeforeReferenceNode === true;"
                         "a.remove(); it2.referenceNode === c && it2.pointerBeforeReferenceNode === true && it2.nextNode() === c"));
    /* removing an ancestor of the reference node */
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var it3 = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT); while (it3.nextNode() !== b); a.remove();"
                         "it3.referenceNode === r && it3.nextNode() === c"));
    /* removing the root's subtree elsewhere or the root itself does not move the reference */
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var it4 = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT); it4.nextNode(); r.remove(); it4.referenceNode === r"));
    env_free(e);
}

EMB_TEST(trav_node_iterator_removal_while_filtering)
{
    env e = env_new();
    EMB_CHECK(js_true(e, k_tree));
    EMB_CHECK(js_true(e, "var gone = false; var it = document.createNodeIterator(r, NodeFilter.SHOW_ELEMENT, { acceptNode(n) {"
                         "  if (n === c && !gone) { gone = true; c.remove() } return NodeFilter.FILTER_ACCEPT } });"
                         "it.nextNode(); it.nextNode(); it.nextNode();"
                         "var got = it.nextNode(); got === c && gone && it.referenceNode === b && it.pointerBeforeReferenceNode === false"));
    EMB_CHECK(js_true(e, "it.nextNode() === d"));
    env_free(e);
}

static void iterator_cycle(void)
{
    env e = env_new();
    (void)js_true(e, k_tree);
    (void)js_true(e, "var its = []; for (let i = 0; i < 20; i++) { its.push(document.createNodeIterator(r)); its.push(document.createTreeWalker(r)); }"
                     "its.forEach(i => i.nextNode && i.nextNode()); true");
    (void)js_true(e, "c.remove(); true");
    env_free(e);
}

EMB_TEST(trav_iterators_are_released_with_the_context)
{
    iterator_cycle(); /* warm-up: interns the atoms and sizes the runtime's tables */
    size_t base = embi_mem_live();
    iterator_cycle();
    iterator_cycle();
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(trav_node_iterator_filter_calling_the_other_method_leaves_the_outer_call_intact)
{
    env e = env_new();
    EMB_CHECK(js_true(e, k_tree));
    /* the nested previousNode() throws InvalidStateError before it touches the in-flight state; the filter removes the
     * candidate, and the outer call completes with the pre-removing steps applied to its own candidate */
    EMB_CHECK(js_true(e, "var nm = ''; var it = document.createNodeIterator(r, 1, function (n) {"
                         "  if (n.id === 'a') { try { it.previousNode() } catch (x) { nm = x.name } n.remove(); }"
                         "  return 1 });"
                         "it.nextNode(); var got = it.nextNode();"
                         "nm === 'InvalidStateError' && got.id === 'a' && it.referenceNode === r && it.pointerBeforeReferenceNode === false"));
    EMB_CHECK(js_true(e, "it.nextNode().id === 'c' && it.nextNode().id === 'd' && it.nextNode() === null"));
    /* the same from previousNode() calling nextNode() */
    EMB_CHECK(js_true(e, "var nm2 = ''; var it2 = document.createNodeIterator(r, 1, function (n) {"
                         "  if (n.id === 'c' && !nm2) { try { it2.nextNode() } catch (x) { nm2 = x.name } } return 1 });"
                         "while (it2.nextNode()) {} var back = it2.previousNode(); nm2 === 'InvalidStateError' && back.id === 'd'"));
    env_free(e);
}

EMB_TEST_MAIN()
