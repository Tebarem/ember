/*
 * The selector state hook (W5d, css/selector.h embi_sel_hooks.state): the dynamic pseudo-classes from the pointer and the
 * focus state, the form pseudo-classes from the attributes, and the simple constraint validation.
 */
#include "emb_test.h"

#include "dom/dom.h"
#include "platform/platform.h"
#include "pf_test.h"

#include <stdio.h>

EMB_TEST(dynamic_state_follows_hover_active_and_focus)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *form = pf_el(doc, &body->base, "form");
    embi_element *input = pf_el(doc, &form->base, "input");
    embi_element *span = pf_el(doc, &body->base, "span");

    embi_pf_state st = {.hover = span, .active = nullptr, .focus = input, .focus_visible = true};
    EMB_CHECK(embi_pf_sel_state(&st, body, EMBI_PC_HOVER));  /* ancestor of the hovered element */
    EMB_CHECK(!embi_pf_sel_state(&st, form, EMBI_PC_HOVER)); /* not an ancestor of span */
    EMB_CHECK(embi_pf_sel_state(&st, span, EMBI_PC_HOVER));
    EMB_CHECK(!embi_pf_sel_state(&st, span, EMBI_PC_ACTIVE));
    EMB_CHECK(embi_pf_sel_state(&st, input, EMBI_PC_FOCUS));
    EMB_CHECK(embi_pf_sel_state(&st, input, EMBI_PC_FOCUS_VISIBLE));
    EMB_CHECK(embi_pf_sel_state(&st, form, EMBI_PC_FOCUS_WITHIN));
    EMB_CHECK(!embi_pf_sel_state(&st, span, EMBI_PC_FOCUS_WITHIN));

    st.focus_visible = false; /* a pointer focus */
    EMB_CHECK(!embi_pf_sel_state(&st, input, EMBI_PC_FOCUS_VISIBLE));
    EMB_CHECK(embi_pf_sel_state(&st, input, EMBI_PC_FOCUS));

    st.active = input;
    EMB_CHECK(embi_pf_sel_state(&st, form, EMBI_PC_ACTIVE));
    embi_document_free(doc);
}

EMB_TEST(form_state_from_attributes)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *box = pf_el(doc, &body->base, "input");
    pf_attr(box, "type", "checkbox");
    pf_attr(box, "checked", "");
    embi_element *off = pf_el(doc, &body->base, "input");
    pf_attr(off, "disabled", "");
    embi_element *text = pf_el(doc, &body->base, "input");
    pf_attr(text, "placeholder", "name");
    embi_element *area = pf_el(doc, &body->base, "textarea");
    pf_attr(area, "readonly", "");
    embi_pf_state st = {0};

    EMB_CHECK(embi_pf_sel_state(&st, box, EMBI_PC_CHECKED));
    EMB_CHECK(embi_pf_sel_state(&st, off, EMBI_PC_DISABLED));
    EMB_CHECK(!embi_pf_sel_state(&st, off, EMBI_PC_ENABLED));
    EMB_CHECK(embi_pf_sel_state(&st, box, EMBI_PC_ENABLED));
    EMB_CHECK(embi_pf_sel_state(&st, text, EMBI_PC_PLACEHOLDER_SHOWN));
    EMB_CHECK(!embi_pf_sel_state(&st, box, EMBI_PC_PLACEHOLDER_SHOWN));
    EMB_CHECK(embi_pf_sel_state(&st, area, EMBI_PC_READ_ONLY));
    EMB_CHECK(!embi_pf_sel_state(&st, area, EMBI_PC_READ_WRITE));
    EMB_CHECK(embi_pf_sel_state(&st, text, EMBI_PC_READ_WRITE));
    EMB_CHECK(embi_pf_sel_state(&st, body, EMBI_PC_READ_ONLY)); /* a non-control is read-only */
    embi_document_free(doc);
}

EMB_TEST(validity_of_simple_constraints)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *req = pf_el(doc, &body->base, "input");
    pf_attr(req, "required", "");
    embi_element *mail = pf_el(doc, &body->base, "input");
    pf_attr(mail, "type", "email");
    pf_attr(mail, "value", "bad");
    embi_element *mail_ok = pf_el(doc, &body->base, "input");
    pf_attr(mail_ok, "type", "email");
    pf_attr(mail_ok, "value", "a@b.c");
    embi_element *num = pf_el(doc, &body->base, "input");
    pf_attr(num, "type", "number");
    pf_attr(num, "min", "5");
    pf_attr(num, "value", "3");
    embi_element *ok_num = pf_el(doc, &body->base, "input");
    pf_attr(ok_num, "type", "number");
    pf_attr(ok_num, "min", "5");
    pf_attr(ok_num, "max", "10");
    pf_attr(ok_num, "value", "7");
    embi_element *max = pf_el(doc, &body->base, "input");
    pf_attr(max, "maxlength", "3");
    pf_attr(max, "value", "abcd");
    embi_element *url = pf_el(doc, &body->base, "input");
    pf_attr(url, "type", "url");
    pf_attr(url, "value", "https://example.com");

    EMB_CHECK(!embi_pf_control_valid(req));
    EMB_CHECK(!embi_pf_control_valid(mail));
    EMB_CHECK(embi_pf_control_valid(mail_ok));
    EMB_CHECK(!embi_pf_control_valid(num));
    EMB_CHECK(embi_pf_control_valid(ok_num));
    EMB_CHECK(!embi_pf_control_valid(max));
    EMB_CHECK(embi_pf_control_valid(url));

    embi_pf_state st = {0};
    EMB_CHECK(embi_pf_sel_state(&st, mail, EMBI_PC_INVALID));
    EMB_CHECK(!embi_pf_sel_state(&st, mail, EMBI_PC_VALID));
    EMB_CHECK(embi_pf_sel_state(&st, mail_ok, EMBI_PC_VALID));
    embi_document_free(doc);
}

EMB_TEST(unknown_input_type_is_text)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *in = pf_el(doc, &body->base, "input");
    pf_attr(in, "type", "bogus"); /* not a type keyword: the Text state */
    pf_attr(in, "placeholder", "p");
    pf_attr(in, "maxlength", "2");
    pf_attr(in, "value", "abc");
    embi_pf_state st = {0};
    EMB_CHECK(!embi_pf_sel_state(&st, in, EMBI_PC_READ_ONLY));
    EMB_CHECK(!embi_pf_control_valid(in)); /* maxlength applies to a text-state input */
    pf_attr(in, "value", "");
    EMB_CHECK(embi_pf_sel_state(&st, in, EMBI_PC_PLACEHOLDER_SHOWN));
    embi_document_free(doc);
}

EMB_TEST(number_grammar_is_the_html_one)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *in = pf_el(doc, &body->base, "input");
    pf_attr(in, "type", "number");
    static const struct {
        const char *v;
        bool ok;
    } cases[] = {{"5", true},    {"-5", true},    {"5.25", true}, {".5", true},   {"1e3", true},  {"1E-3", true},
                 {"+5", false},  {"inf", false},  {"nan", false}, {"0x10", false}, {" 5", false}, {"5 ", false},
                 {"5.", false},  {"1e", false},   {"-", false},   {"--5", false}, {"1,5", false}};
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pf_attr(in, "value", cases[i].v);
        if (embi_pf_control_valid(in) != cases[i].ok) {
            EMB_CHECK(!"number grammar case");
            fprintf(stderr, "  value \"%s\" expected %s\n", cases[i].v, cases[i].ok ? "valid" : "invalid");
        }
    }
    pf_attr(in, "min", "+1"); /* an invalid min is ignored */
    pf_attr(in, "value", "0");
    EMB_CHECK(embi_pf_control_valid(in));
    embi_document_free(doc);
}

EMB_TEST_MAIN()
