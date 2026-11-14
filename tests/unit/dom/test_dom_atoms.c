#include "emb_test.h"

#include "dom/dom.h"

EMB_TEST(atoms_static_ids_match_enum)
{
    embi_dom_init();
    EMB_CHECK(embi_atom_intern("div", 3) == EMBI_ATOM_div);
    EMB_CHECK(embi_atom_intern("template", 8) == EMBI_ATOM_template);
    EMB_CHECK(embi_atom_intern("id", 2) == EMBI_ATOM_id);
    EMB_CHECK(embi_atom_intern("class", 5) == EMBI_ATOM_class);
    EMB_CHECK(embi_atom_intern("foreignObject", 13) == EMBI_ATOM_foreignObject);
    EMB_CHECK(embi_atom_intern("annotation-xml", 14) == EMBI_ATOM_annotation_xml);
    EMB_CHECK(embi_atom_intern("xlink:href", 10) == EMBI_ATOM_xlink_href);
    EMB_CHECK(EMBI_ATOM_STATIC_COUNT > 600);
}

EMB_TEST(atoms_static_table_round_trips)
{
    embi_dom_init();
    for (uint32_t id = 1; id < EMBI_ATOM_STATIC_COUNT; id++) {
        embi_str s = embi_atom_str(id);
        EMB_REQUIRE(s.len > 0);
        EMB_CHECK(embi_atom_intern(s.ptr, s.len) == id);
    }
}

EMB_TEST(atoms_case_variants_are_distinct)
{
    EMB_CHECK(EMBI_ATOM_foreignObject != EMBI_ATOM_foreignobject);
    EMB_CHECK(EMBI_ATOM_altGlyph != EMBI_ATOM_altglyph);
    EMB_CHECK(EMBI_ATOM_viewBox != EMBI_ATOM_viewbox);
    EMB_CHECK(EMBI_ATOM_definitionURL != EMBI_ATOM_definitionurl);
    EMB_CHECK(EMBI_ATOM_xlink_href != EMBI_ATOM_href);
}

EMB_TEST(atoms_reinitialize_after_shutdown)
{
    embi_dom_init();
    embi_atom_shutdown();
    embi_dom_init();
    EMB_CHECK(embi_atom_intern("span", 4) == EMBI_ATOM_span);
    EMB_CHECK(embi_atom_intern("body", 4) == EMBI_ATOM_body);
}

EMB_TEST(struct_layout_budgets)
{
    EMB_CHECK_EQ_INT(sizeof(embi_node), 64);
    EMB_CHECK(sizeof(embi_element) <= 96);
    EMB_CHECK_EQ_INT(sizeof(embi_attr), 32);
    EMB_CHECK(sizeof(embi_chardata) <= 96);
}

EMB_TEST(atoms_rejects_malformed_utf8_in_names)
{
    embi_dom_init();
    EMB_CHECK(!embi_dom_is_name((embi_str){"a\xFF", 2}, true));
    EMB_CHECK(!embi_dom_is_name((embi_str){"\xE2\x82", 2}, true));
    EMB_CHECK(embi_dom_is_name((embi_str){"a\xEF\xBF\xBD", 4}, true));
    EMB_CHECK(embi_dom_is_name((embi_str){"caf\xC3\xA9", 5}, true));
}

EMB_TEST(atoms_find_does_not_insert)
{
    embi_dom_init();
    EMB_CHECK(embi_atom_find("never-interned-xyz", 18) == 0);
    EMB_CHECK(embi_atom_find("never-interned-xyz", 18) == 0);
    EMB_CHECK(embi_atom_find("div", 3) == EMBI_ATOM_div);
    embi_document *d = embi_document_new(true);
    EMB_CHECK(embi_get_element_by_id(d, EMBI_STR("never-interned-xyz")) == nullptr);
    EMB_CHECK(embi_atom_find("never-interned-xyz", 18) == 0);
    embi_document_free(d);
}

EMB_TEST_MAIN()
