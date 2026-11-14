#include "emb_test.h"

#include "dom/dom.h"

#define S(lit) EMBI_STR(lit)

#define XLINK_URI "http://www.w3.org/1999/xlink"
#define XML_URI "http://www.w3.org/XML/1998/namespace"
#define XMLNS_URI "http://www.w3.org/2000/xmlns/"
#define SVG_URI "http://www.w3.org/2000/svg"

static embi_element *make(embi_document *d, embi_str name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, name, &e) == EMBI_DOM_OK);
    return e;
}

static embi_dstr v8(embi_document *d, const char *utf8)
{
    embi_str s = {utf8, (uint32_t)SDL_strlen(utf8)};
    return embi_dstr_from_utf8(d->slab, s);
}

static bool value_is(const embi_attr *at, const char *utf8)
{
    if (!at) {
        return false;
    }
    embi_str s = {utf8, (uint32_t)SDL_strlen(utf8)};
    return embi_dstr_eq_utf8(at->value, s);
}

EMB_TEST(attr_set_get_replace_remove)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("div"));
    EMB_CHECK(embi_el_set_attribute(el, S("data-x"), v8(d, "1")) == EMBI_DOM_OK);
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("data-x")), "1"));
    EMB_CHECK(embi_el_has_attribute(el, S("data-x")));
    EMB_CHECK(embi_el_set_attribute(el, S("data-x"), v8(d, "2")) == EMBI_DOM_OK);
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("data-x")), "2"));
    EMB_CHECK_EQ_INT(el->attrs->len, 1);
    EMB_CHECK(embi_el_remove_attribute(el, S("data-x")));
    EMB_CHECK(!embi_el_has_attribute(el, S("data-x")));
    EMB_CHECK(!embi_el_remove_attribute(el, S("data-x")));
    EMB_CHECK(el->attrs == nullptr);
    embi_document_free(d);
}

EMB_TEST(attr_invalid_names_are_rejected)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("div"));
    EMB_CHECK(embi_el_set_attribute(el, S("a=b"), v8(d, "x")) == EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_el_set_attribute(el, S("a b"), v8(d, "x")) == EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_el_set_attribute(el, S(""), v8(d, "x")) == EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_el_set_attribute(el, S("ok-name.1"), v8(d, "x")) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_element(d, S("bad name"), &el) == EMBI_DOM_ERR_INVALID_CHAR);
    embi_document_free(d);
}

EMB_TEST(attr_html_names_lowercase_only_in_html_elements)
{
    embi_document *d = embi_document_new(true);
    embi_element *div = make(d, S("DIV"));
    EMB_CHECK(div->base.name == embi_atom_intern("div", 3));
    EMB_CHECK(embi_el_set_attribute(div, S("ID"), v8(d, "Main")) == EMBI_DOM_OK);
    EMB_CHECK(value_is(embi_el_get_attribute(div, S("id")), "Main"));
    EMB_CHECK(value_is(embi_el_get_attribute(div, S("Id")), "Main"));
    EMB_CHECK(embi_el_id(div) == embi_atom_intern("Main", 4));

    embi_element *svg = nullptr;
    EMB_CHECK(embi_dom_create_element_ns(d, true, S(SVG_URI), S("svg"), &svg) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(svg, S("viewBox"), v8(d, "0 0 1 1")) == EMBI_DOM_OK);
    EMB_CHECK(value_is(embi_el_get_attribute(svg, S("viewBox")), "0 0 1 1"));
    EMB_CHECK(embi_el_get_attribute(svg, S("viewbox")) == nullptr);
    EMB_CHECK(embi_str_eq(embi_el_namespace_uri(svg), S(SVG_URI)));

    embi_document *xml = embi_document_new(false);
    embi_element *xe = make(xml, S("Item"));
    EMB_CHECK(xe->base.name == embi_atom_intern("Item", 4));
    EMB_CHECK(embi_el_set_attribute(xe, S("Attr"), v8(xml, "v")) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_get_attribute(xe, S("Attr")) != nullptr);
    EMB_CHECK(embi_el_get_attribute(xe, S("attr")) == nullptr);
    embi_document_free(xml);
    embi_document_free(d);
}

EMB_TEST(attr_namespaced_set_get_and_prefix)
{
    embi_document *d = embi_document_new(true);
    embi_element *svg = nullptr;
    EMB_CHECK(embi_dom_create_element_ns(d, true, S(SVG_URI), S("use"), &svg) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute_ns(svg, true, S(XLINK_URI), S("xlink:href"), v8(d, "#a")) ==
              EMBI_DOM_OK);
    const embi_attr *at = embi_el_get_attribute_ns(svg, true, S(XLINK_URI), S("href"));
    EMB_REQUIRE(at != nullptr);
    EMB_CHECK(at->prefix == embi_atom_intern("xlink", 5));
    EMB_CHECK(at->ns == EMBI_NS_XLINK);
    EMB_CHECK(value_is(at, "#a"));
    EMB_CHECK(embi_el_get_attribute(svg, S("xlink:href")) == at);
    EMB_CHECK(embi_el_get_attribute(svg, S("href")) == nullptr);
    EMB_CHECK(embi_el_has_attribute_ns(svg, true, S(XLINK_URI), S("href")));
    EMB_CHECK(!embi_el_has_attribute_ns(svg, false, S(""), S("href")));

    /* Setting the same namespace and local name again keeps one attribute. */
    EMB_CHECK(embi_el_set_attribute_ns(svg, true, S(XLINK_URI), S("xlink:href"), v8(d, "#b")) ==
              EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(svg->attrs->len, 1);
    EMB_CHECK(value_is(embi_el_get_attribute_ns(svg, true, S(XLINK_URI), S("href")), "#b"));

    /* An unknown namespace is kept as an atom. */
    EMB_CHECK(embi_el_set_attribute_ns(svg, true, S("urn:x-test"), S("t:p"), v8(d, "q")) ==
              EMBI_DOM_OK);
    const embi_attr *other = embi_el_get_attribute_ns(svg, true, S("urn:x-test"), S("p"));
    EMB_REQUIRE(other != nullptr);
    EMB_CHECK(other->ns == EMBI_NS_OTHER);
    EMB_CHECK(other->ns_uri == embi_atom_intern("urn:x-test", 10));
    EMB_CHECK(embi_el_get_attribute_ns(svg, false, S(""), S("p")) == nullptr);

    EMB_CHECK(embi_el_remove_attribute_ns(svg, true, S(XLINK_URI), S("href")));
    EMB_CHECK(!embi_el_has_attribute_ns(svg, true, S(XLINK_URI), S("href")));
    embi_document_free(d);
}

EMB_TEST(attr_namespace_errors)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("div"));
    /* A prefix needs a namespace. */
    EMB_CHECK(embi_el_set_attribute_ns(el, false, S(""), S("xlink:href"), v8(d, "x")) ==
              EMBI_DOM_ERR_NAMESPACE);
    /* The xml prefix must be the XML namespace. */
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S(XLINK_URI), S("xml:lang"), v8(d, "en")) ==
              EMBI_DOM_ERR_NAMESPACE);
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S(XML_URI), S("xml:lang"), v8(d, "en")) ==
              EMBI_DOM_OK);
    /* xmlns belongs to the XMLNS namespace only, and that namespace needs xmlns. */
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S(XMLNS_URI), S("foo"), v8(d, "x")) ==
              EMBI_DOM_ERR_NAMESPACE);
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S(XMLNS_URI), S("xmlns:foo"), v8(d, "x")) ==
              EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S("urn:x"), S("xmlns"), v8(d, "x")) ==
              EMBI_DOM_ERR_NAMESPACE);
    /* Malformed qualified names. */
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S(XLINK_URI), S("a b"), v8(d, "x")) ==
              EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S(XLINK_URI), S(":b"), v8(d, "x")) ==
              EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_dom_create_element_ns(d, false, S(""), S("p:q"), &el) ==
              EMBI_DOM_ERR_NAMESPACE);
    /* The XML namespace may carry any prefix on an element. Only the xml prefix is restricted. */
    EMB_CHECK(embi_dom_create_element_ns(d, true, S(XML_URI), S("p:q"), &el) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_element_ns(d, true, S(XLINK_URI), S("xml:q"), &el) ==
              EMBI_DOM_ERR_NAMESPACE);
    EMB_CHECK(embi_dom_create_element_ns(d, true, S("urn:x"), S("a:b"), &el) ==
              EMBI_DOM_OK);
    embi_document_free(d);
}

EMB_TEST(attr_id_cache_follows_the_attribute)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("span"));
    EMB_CHECK(embi_el_id(el) == 0);
    EMB_CHECK(embi_el_set_attribute(el, S("id"), v8(d, "alpha")) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_id(el) == embi_atom_intern("alpha", 5));
    EMB_CHECK(embi_el_set_attribute(el, S("id"), v8(d, "beta")) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_id(el) == embi_atom_intern("beta", 4));
    EMB_CHECK(embi_el_remove_attribute(el, S("id")));
    EMB_CHECK(embi_el_id(el) == 0);
    /* An id in another namespace does not count. */
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S("urn:x"), S("n:id"), v8(d, "gamma")) ==
              EMBI_DOM_OK);
    EMB_CHECK(embi_el_id(el) == 0);
    embi_document_free(d);
}

EMB_TEST(attr_class_tokens_split_on_ascii_space)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("div"));
    uint32_t n = 99;
    EMB_CHECK(embi_el_classes(el, &n) == nullptr);
    EMB_CHECK_EQ_INT(n, 0);
    EMB_CHECK(embi_el_set_attribute(el, S("class"), v8(d, "  a b\tc\n a \f")) == EMBI_DOM_OK);
    const embi_atom *list = embi_el_classes(el, &n);
    EMB_CHECK_EQ_INT(n, 4);
    EMB_REQUIRE(list != nullptr);
    EMB_CHECK(list[0] == embi_atom_intern("a", 1));
    EMB_CHECK(list[1] == embi_atom_intern("b", 1));
    EMB_CHECK(list[2] == embi_atom_intern("c", 1));
    EMB_CHECK(embi_el_has_class(el, embi_atom_intern("c", 1)));
    EMB_CHECK(!embi_el_has_class(el, embi_atom_intern("d", 1)));

    /* The cache follows changes to class. */
    EMB_CHECK(embi_el_set_attribute(el, S("class"), v8(d, "x")) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_has_class(el, embi_atom_intern("x", 1)));
    EMB_CHECK(!embi_el_has_class(el, embi_atom_intern("a", 1)));
    EMB_CHECK(embi_el_remove_attribute(el, S("class")));
    EMB_CHECK(!embi_el_has_class(el, embi_atom_intern("x", 1)));
    embi_document_free(d);
}

EMB_TEST(attr_toggle_follows_force_rules)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("input"));
    bool now = false;
    EMB_CHECK(embi_el_toggle_attribute(el, S("hidden"), false, false, &now) == EMBI_DOM_OK);
    EMB_CHECK(now);
    EMB_CHECK(embi_el_has_attribute(el, S("hidden")));
    EMB_CHECK(embi_el_toggle_attribute(el, S("hidden"), false, false, &now) == EMBI_DOM_OK);
    EMB_CHECK(!now);
    EMB_CHECK(!embi_el_has_attribute(el, S("hidden")));
    /* force false when absent adds nothing. */
    EMB_CHECK(embi_el_toggle_attribute(el, S("hidden"), true, false, &now) == EMBI_DOM_OK);
    EMB_CHECK(!now);
    EMB_CHECK(!embi_el_has_attribute(el, S("hidden")));
    /* force true when present keeps the value. */
    EMB_CHECK(embi_el_set_attribute(el, S("title"), v8(d, "keep")) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_toggle_attribute(el, S("title"), true, true, &now) == EMBI_DOM_OK);
    EMB_CHECK(now);
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("title")), "keep"));
    EMB_CHECK(embi_el_toggle_attribute(el, S("bad name"), false, false, &now) ==
              EMBI_DOM_ERR_INVALID_CHAR);
    embi_document_free(d);
}

EMB_TEST(attr_many_attributes_grow_and_shrink)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("div"));
    char name[16];
    for (int i = 0; i < 200; i++) {
        int n = SDL_snprintf(name, sizeof name, "a%d", i);
        EMB_REQUIRE(embi_el_set_attribute(el, (embi_str){name, (uint32_t)n}, v8(d, name)) ==
                    EMBI_DOM_OK);
    }
    EMB_CHECK_EQ_INT(el->attrs->len, 200);
    bool ok = true;
    for (int i = 0; i < 200; i++) {
        int n = SDL_snprintf(name, sizeof name, "a%d", i);
        const embi_attr *at = embi_el_get_attribute(el, (embi_str){name, (uint32_t)n});
        ok = ok && value_is(at, name);
    }
    EMB_CHECK(ok);
    for (int i = 0; i < 200; i += 2) {
        int n = SDL_snprintf(name, sizeof name, "a%d", i);
        EMB_CHECK(embi_el_remove_attribute(el, (embi_str){name, (uint32_t)n}));
    }
    EMB_CHECK_EQ_INT(el->attrs->len, 100);
    int n = SDL_snprintf(name, sizeof name, "a%d", 7);
    EMB_CHECK(embi_el_get_attribute(el, (embi_str){name, (uint32_t)n}) != nullptr);
    for (int i = 1; i < 200; i += 2) {
        n = SDL_snprintf(name, sizeof name, "a%d", i);
        EMB_CHECK(embi_el_remove_attribute(el, (embi_str){name, (uint32_t)n}));
    }
    EMB_CHECK(el->attrs == nullptr);
    embi_document_free(d);
}

typedef struct attr_log {
    int changes;
    int with_old;
    int last_had_old;
    uint32_t last_old_len;
    embi_atom last_name;
    uint8_t last_ns;
} attr_log;

static void on_attr(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom name,
                    embi_dstr old, bool had_old)
{
    (void)el;
    (void)ns_uri;
    ((attr_log *)ctx)->last_ns = ns;
    attr_log *log = ctx;
    log->changes++;
    log->last_name = name;
    log->last_had_old = had_old;
    log->last_old_len = old.len;
    if (had_old) {
        log->with_old++;
    }
}

EMB_TEST(attr_observer_sees_old_values)
{
    embi_document *d = embi_document_new(true);
    attr_log log = {0, 0, 0, 0, 0, 0};
    embi_observer obs = {&log, nullptr, on_attr, nullptr, nullptr, nullptr, nullptr};
    embi_document_observe(d, &obs);
    embi_element *el = make(d, S("div"));
    EMB_CHECK(embi_el_set_attribute(el, S("data-k"), v8(d, "abc")) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(log.changes, 1);
    EMB_CHECK(!log.last_had_old);
    EMB_CHECK(embi_el_set_attribute(el, S("data-k"), v8(d, "de")) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(log.changes, 2);
    EMB_CHECK(log.last_had_old);
    EMB_CHECK_EQ_INT(log.last_old_len, 3);
    EMB_CHECK(embi_el_remove_attribute(el, S("data-k")));
    EMB_CHECK_EQ_INT(log.changes, 3);
    EMB_CHECK(log.last_had_old);
    EMB_CHECK_EQ_INT(log.last_old_len, 2);
    EMB_CHECK(log.last_name == embi_atom_intern("data-k", 6));
    EMB_CHECK(!embi_el_remove_attribute(el, S("data-k")));
    EMB_CHECK_EQ_INT(log.changes, 3);
    embi_document_unobserve(d, &log);
    EMB_CHECK(embi_el_set_attribute(el, S("data-k"), v8(d, "x")) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(log.changes, 3);
    embi_document_free(d);
}

EMB_TEST(attr_observer_reports_namespace_after_removal)
{
    embi_document *d = embi_document_new(true);
    attr_log log = {0, 0, 0, 0, 0, 0};
    embi_observer obs = {&log, nullptr, on_attr, nullptr, nullptr, nullptr, nullptr};
    embi_document_observe(d, &obs);
    embi_element *el = make(d, S("div"));
    EMB_CHECK(embi_el_set_attribute_ns(el, true, S("http://www.w3.org/1999/xlink"), S("xlink:href"),
                                       v8(d, "u")) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(log.last_ns, EMBI_NS_XLINK);
    EMB_CHECK(embi_el_remove_attribute_ns(el, true, S("http://www.w3.org/1999/xlink"), S("href")));
    EMB_CHECK_EQ_INT(log.last_ns, EMBI_NS_XLINK);
    EMB_CHECK(log.last_name == embi_atom_intern("href", 4));
    embi_document_unobserve(d, &log);
    embi_document_free(d);
}

EMB_TEST(attr_values_are_copied_from_the_caller)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, S("div"));
    embi_dstr tmp = v8(d, "transient");
    EMB_CHECK(embi_el_set_attribute(el, S("title"), tmp) == EMBI_DOM_OK);
    embi_dstr_free(tmp);
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("title")), "transient"));
    embi_document_free(d);
}

/* setAttribute finds an existing prefixed attribute by its qualified name and updates it in place,
 * keeping its namespace and prefix. */
EMB_TEST(attr_set_finds_prefixed_attribute_by_qname)
{
    embi_document *d = embi_document_new(true);
    embi_element *e = make(d, S("a"));
    EMB_CHECK(embi_el_set_attribute_ns(e, true, S(XLINK_URI), S("xlink:href"), v8(d, "one")) ==
              EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(e, S("xlink:href"), v8(d, "two")) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(embi_el_attr_count(e), 1);
    const embi_attr *at = embi_el_attr_at(e, 0);
    EMB_CHECK(at->ns == EMBI_NS_XLINK);
    EMB_CHECK(at->prefix == embi_atom_intern("xlink", 5));
    EMB_CHECK(at->name == embi_atom_intern("href", 4));
    EMB_CHECK(value_is(at, "two"));
    /* The namespaced lookup sees the same attribute, and getAttribute by qname agrees. */
    EMB_CHECK(value_is(embi_el_get_attribute_ns(e, true, S(XLINK_URI), S("href")), "two"));
    EMB_CHECK(value_is(embi_el_get_attribute(e, S("xlink:href")), "two"));
    /* A plain qname that only matches the local name does not match the prefixed attribute. */
    EMB_CHECK(embi_el_get_attribute(e, S("href")) == nullptr);
    EMB_CHECK(embi_el_set_attribute(e, S("href"), v8(d, "three")) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(embi_el_attr_count(e), 2);
    EMB_CHECK(value_is(embi_el_get_attribute(e, S("xlink:href")), "two"));
    EMB_CHECK(value_is(embi_el_get_attribute(e, S("href")), "three"));
    embi_document_free(d);
}

typedef struct attr_probe {
    bool id_seen;
    bool class_seen;
} attr_probe;

static void probe_caches(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom name,
                         embi_dstr old, bool had_old)
{
    (void)ns;
    (void)ns_uri;
    (void)old;
    (void)had_old;
    attr_probe *p = ctx;
    if (name == EMBI_ATOM_id) {
        p->id_seen = embi_el_id(el) == embi_atom_intern("new", 3);
    } else if (name == EMBI_ATOM_class) {
        uint32_t n = 0;
        const embi_atom *c = embi_el_classes(el, &n);
        p->class_seen = n == 1 && c[0] == embi_atom_intern("beta", 4);
    }
}

/* The id and class caches are current when observers run, so a callback sees the new values. */
EMB_TEST(attr_caches_are_current_inside_observers)
{
    embi_document *d = embi_document_new(true);
    attr_probe p = {false, false};
    embi_observer obs = {&p, nullptr, probe_caches, nullptr, nullptr, nullptr, nullptr};
    embi_document_observe(d, &obs);
    embi_element *el = make(d, S("div"));
    EMB_CHECK(embi_el_set_attribute(el, S("id"), v8(d, "new")) == EMBI_DOM_OK);
    EMB_CHECK(p.id_seen);
    EMB_CHECK(embi_el_set_attribute(el, S("class"), v8(d, "beta")) == EMBI_DOM_OK);
    EMB_CHECK(p.class_seen);
    embi_document_unobserve(d, &p);
    embi_document_free(d);
}

/* The parser's one-step element: the values live in the attribute block itself. Every later
 * change has to cope with that (replace, remove, growth, clone, destruction). */
static void inline_values_cycle(void)
{
    embi_document *d = embi_document_new(true);
    embi_attr_init init[4] = {
        {EMBI_ATOM_id, S("main")},
        {EMBI_ATOM_class, S("a  b")},
        {EMBI_ATOM_title, S("caf\xC3\xA9 \xE2\x82\xAC")}, /* a wide value */
        {embi_atom_intern("data-empty", 10), S("")},
    };
    embi_element *el = embi_new_element_attrs(d, EMBI_NS_HTML, EMBI_ATOM_div, 4, init);
    EMB_CHECK_EQ_INT(embi_el_attr_count(el), 4);
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("id")), "main"));
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("title")), "caf\xC3\xA9 \xE2\x82\xAC"));
    EMB_CHECK(embi_el_get_attribute(el, S("title"))->value.wide);
    EMB_CHECK(embi_el_get_attribute(el, S("data-empty"))->value.len == 0);
    EMB_CHECK(embi_el_id(el) == embi_atom_intern("main", 4));
    uint32_t nc = 0;
    EMB_CHECK(embi_el_classes(el, &nc) != nullptr && nc == 2);

    /* Replace and remove entries whose values are inline. */
    EMB_CHECK(embi_el_set_attribute(el, S("title"), v8(d, "plain")) == EMBI_DOM_OK);
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("title")), "plain"));
    EMB_CHECK(embi_el_set_attribute(el, S("id"), v8(d, "second")) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_id(el) == embi_atom_intern("second", 6));
    EMB_CHECK(embi_el_remove_attribute(el, S("class")));
    EMB_CHECK(embi_el_id(el) == embi_atom_intern("second", 6));

    /* Growing the block moves the values that were inline into their own blocks. */
    char name[16];
    for (int i = 0; i < 12; i++) {
        SDL_snprintf(name, sizeof name, "g%d", i);
        EMB_CHECK(embi_el_set_attribute(el, (embi_str){name, (uint32_t)SDL_strlen(name)},
                                        v8(d, "v")) == EMBI_DOM_OK);
    }
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("title")), "plain"));
    EMB_CHECK(value_is(embi_el_get_attribute(el, S("id")), "second"));
    EMB_CHECK(embi_el_get_attribute(el, S("data-empty"))->value.len == 0);
    EMB_CHECK(embi_el_remove_attribute(el, S("id")));
    EMB_CHECK(embi_el_id(el) == 0);

    /* A block that kept its inline values is cloned and destroyed. */
    embi_element *el2 = embi_new_element_attrs(d, EMBI_NS_HTML, EMBI_ATOM_span, 4, init);
    embi_node *copy = embi_clone_node(&el2->base, false);
    EMB_CHECK(value_is(embi_el_get_attribute((embi_element *)copy, S("title")),
                       "caf\xC3\xA9 \xE2\x82\xAC"));
    embi_destroy_detached(copy);
    embi_destroy_detached(&el2->base);

    /* The lazy id reaches the id map too. */
    embi_element *el3 = embi_new_element_attrs(d, EMBI_NS_HTML, EMBI_ATOM_div, 1,
                                               &(embi_attr_init){EMBI_ATOM_id, S("findme")});
    EMB_CHECK(embi_append_child(&d->base, &el3->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_get_element_by_id(d, S("findme")) == el3);
    EMB_CHECK(embi_el_set_attribute(el3, S("id"), v8(d, "moved")) == EMBI_DOM_OK);
    EMB_CHECK(embi_get_element_by_id(d, S("findme")) == nullptr);
    EMB_CHECK(embi_get_element_by_id(d, S("moved")) == el3);

    embi_document_free(d);
}

EMB_TEST(parser_element_attrs_store_values_inline)
{
    inline_values_cycle(); /* the atoms it interns stay in the atom table */
    size_t before = embi_mem_live();
    inline_values_cycle();
    EMB_CHECK(embi_mem_live() == before);
}

/* Adopting a parser-style element (inline values) into another document, then freeing the source
 * document, must keep the block alive and never treat an interior pointer as a slab payload. */
EMB_TEST(adopt_element_with_inline_values)
{
    embi_document *a = embi_document_new(true);
    embi_document *b = embi_document_new(true);
    embi_attr_init init[2] = {
        {EMBI_ATOM_id, S("hello-world")},
        {EMBI_ATOM_class, S("abc def")},
    };
    embi_element *e = embi_new_element_attrs(a, EMBI_NS_HTML, EMBI_ATOM_div, 2, init);
    EMB_CHECK(embi_adopt_node(b, &e->base) == EMBI_DOM_OK);
    embi_document_free(a);
    EMB_CHECK(value_is(embi_el_get_attribute(e, S("id")), "hello-world"));
    EMB_CHECK(value_is(embi_el_get_attribute(e, S("class")), "abc def"));
    EMB_CHECK(embi_el_id(e) == embi_atom_intern("hello-world", 11));
    EMB_CHECK(embi_append_child(&b->base, &e->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_get_element_by_id(b, S("hello-world")) == e);
    embi_document_free(b);
}

EMB_TEST_MAIN()
