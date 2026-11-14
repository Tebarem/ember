/*
 * The form controls (W5e, bindings/impl_forms.c): value and checkedness with their dirty flags, the value sanitization and
 * the type keywords, radio groups, text selection, the edit pipeline (beforeinput, input, cancelation) and the key defaults,
 * FormData from a form, submission (submit event, navigation through the navigate hook), reset, the activation of checkbox,
 * radio and submit buttons, and constraint validation (invalid events, ValidityState, custom messages).
 */
#include "emb_test.h"

#include "base/base.h"
#include "bindings/activation.h"
#include "bindings/bind.h"
#include "bindings/impl.h"
#include "bindings/impl_forms.h"
#include "bindings/impl_input.h"
#include "dom/dom.h"
#include "js/heap.h"
#include "js/jobs.h"
#include "js/js.h"
#include "js/runtime.h"
#include "paint/paint.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
} env;

static const char k_url[] = "http://127.0.0.1/start.html";

static embi_str url_of(void *user)
{
    (void)user;
    return (embi_str){k_url, (uint32_t)strlen(k_url)};
}

/* The navigate hook of the tests: counts the navigations and remembers the last URL. */
static char g_url[512];
static int g_nav;

static bool capture_nav(void *user, embi_str url, bool replace)
{
    (void)user;
    (void)replace;
    g_nav++;
    uint32_t n = url.len < sizeof g_url - 1 ? url.len : (uint32_t)(sizeof g_url - 1);
    SDL_memcpy(g_url, url.ptr, n);
    g_url[n] = '\0';
    return true;
}

/* The log hook of the tests: collects the console lines. */
static char g_console[2048];

static void capture_log(void *user, int level, embi_str text)
{
    (void)user;
    (void)level;
    size_t n = strlen(g_console);
    if (n + text.len + 2 < sizeof g_console) {
        memcpy(g_console + n, text.ptr, text.len);
        g_console[n + text.len] = '|';
        g_console[n + text.len + 1] = '\0';
    }
}

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
    embi_bind_host h;
    memset(&h, 0, sizeof h);
    h.navigate = capture_nav;
    h.url = url_of;
    h.log = capture_log;
    embi_bind_set_host(e.c, &h);
    embi_forms_ready();
    g_console[0] = '\0';
    g_nav = 0;
    g_url[0] = '\0';
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

static embi_node *by_id(env e, const char *id)
{
    embi_element *el = embi_get_element_by_id(e.doc, (embi_str){id, (uint32_t)strlen(id)});
    return el != nullptr ? &el->base : nullptr;
}

EMB_TEST(bind_form_value_default_and_dirty)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=a value=\"x\"><textarea id=t>tx</textarea>'; true"));
    EMB_CHECK(js_true(e, "var a = document.getElementById('a'); a.value === 'x' && a.defaultValue === 'x'"));
    /* A script value makes the value dirty: the attribute keeps the default. */
    EMB_CHECK(js_true(e, "a.value = 'y'; a.value === 'y' && a.getAttribute('value') === 'x'"));
    /* Textarea: the default value is its text content. */
    EMB_CHECK(js_true(e, "var t = document.getElementById('t'); t.value === 'tx' && t.defaultValue === 'tx'"));
    EMB_CHECK(js_true(e, "t.value = 'changed'; t.value === 'changed' && t.textContent === 'tx'"));
    env_free(e);
}

EMB_TEST(bind_form_type_keywords_and_sanitization)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=i type=BOGUS><input id=c type=CHECKBOX>"
                         "<input id=n type=number><input id=t type=text><input id=r type=range min=0 max=10>"
                         "<input id=col type=color>'; true"));
    EMB_CHECK(js_true(e, "document.getElementById('i').type === 'text' && document.getElementById('c').type === 'checkbox'"));
    EMB_CHECK(js_true(e, "var n = document.getElementById('n'); n.value = 'abc'; n.value === '' && "
                         "(n.value = '12.5e1', n.value === '12.5e1') && (n.value = '5.', n.value === '')"));
    EMB_CHECK(js_true(e, "var t = document.getElementById('t'); t.value = 'a\\nb\\rc'; t.value === 'abc'"));
    EMB_CHECK(js_true(e, "var r = document.getElementById('r'); r.value = '42'; r.value === '10' && "
                         "(r.value = '-5', r.value === '0')"));
    EMB_CHECK(js_true(e, "var col = document.getElementById('col'); col.value === '#000000' && "
                         "(col.value = '#ABCDEF', col.value === '#abcdef') && (col.value = 'red', col.value === '#000000')"));
    env_free(e);
}

EMB_TEST(bind_form_checkbox_and_radio_group)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f><input type=checkbox id=k>"
                         "<input type=radio name=g id=r1 checked><input type=radio name=g id=r2></form>"
                         "<input type=radio name=g id=r3>'; true"));
    EMB_CHECK(js_true(e, "var k = document.getElementById('k'); k.checked === false && "
                         "(k.checked = true, k.checked === true) && k.getAttribute('checked') === null"));
    /* A radio group is the radios of the same name and form owner: r3 is outside the form, so it is a separate group. */
    EMB_CHECK(js_true(e, "var r1 = document.getElementById('r1'), r2 = document.getElementById('r2'), "
                         "r3 = document.getElementById('r3'); r1.checked === true && "
                         "(r2.checked = true, r1.checked === false && r2.checked === true) && r3.checked === false && "
                         "(r3.checked = true, r2.checked === true)"));
    env_free(e);
}

EMB_TEST(bind_form_checkbox_click_fires_input_and_change)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input type=checkbox id=k>'; true"));
    EMB_CHECK(js_true(e, "window.ev = []; var k = document.getElementById('k'); "
                         "['input', 'change'].forEach(function (t) { k.addEventListener(t, function () { ev.push(t); }); }); "
                         "k.click(); ev.join(',') === 'input,change' && k.checked === true"));
    /* A canceled click restores the checkedness. */
    EMB_CHECK(js_true(e, "k.addEventListener('click', function (x) { x.preventDefault(); }); k.click(); k.checked === true"));
    env_free(e);
}

EMB_TEST(bind_form_selection_and_range)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=s value=\"hello\"><textarea id=ta>abc</textarea>'; true"));
    EMB_CHECK(js_true(e, "var s = document.getElementById('s'); s.setSelectionRange(1, 3); "
                         "s.selectionStart === 1 && s.selectionEnd === 3 && s.selectionDirection === 'none'"));
    EMB_CHECK(js_true(e, "s.setSelectionRange(4, 2, 'backward'); s.selectionStart === 2 && s.selectionEnd === 4 && "
                         "s.selectionDirection === 'backward'"));
    /* Setting selectionEnd below the start moves the start down with it (the caret was at 3 after the text). */
    EMB_CHECK(js_true(e, "var ta = document.getElementById('ta'); ta.selectionEnd = 2; ta.selectionStart === 2 && "
                         "ta.selectionEnd === 2"));
    EMB_CHECK(js_true(e, "var cb = document.createElement('input'); cb.type = 'checkbox'; "
                         "var threw = false; try { cb.setSelectionRange(0, 1); } catch (x) { threw = x.name === "
                         "'InvalidStateError'; } threw"));
    env_free(e);
}

EMB_TEST(bind_form_edit_pipeline_and_keys)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"ab\"><input id=ro value=\"ro\" readonly>'; true"));
    EMB_CHECK(js_true(e, "window.log = []; var t = document.getElementById('t'); "
                         "t.addEventListener('beforeinput', function (x) { log.push('before:' + x.inputType + ':' + x.data); });"
                         "t.addEventListener('input', function (x) { log.push('input:' + x.inputType + ':' + x.data); });"
                         "t.setSelectionRange(1, 1); true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"xy", 2}, false));
    EMB_CHECK(js_true(e, "t.value === 'axyb'"));
    EMB_CHECK(js_true(e, "t.selectionStart === 3"));
    EMB_CHECK(js_true(e, "log.length === 2"));
    EMB_CHECK(js_true(e, "log[0] === 'before:insertText:xy'"));
    EMB_CHECK(js_true(e, "log[1].indexOf('input:insertText') === 0"));
    EMB_CHECK(js_true(e, "log[1].length > 0 && log[1].charAt(log[1].length - 1) === 'y'"));
    /* Backspace removes the character before the caret, Delete the one after it. */
    EMB_CHECK(embi_forms_key(e.c, t, "Backspace", false, false, false));
    EMB_CHECK(js_true(e, "t.value === 'axb' && t.selectionStart === 2"));
    EMB_CHECK(embi_forms_key(e.c, t, "Delete", false, false, false));
    EMB_CHECK(js_true(e, "t.value === 'ax' && t.selectionStart === 2"));
    /* Home, arrows and Ctrl+A move or select without editing. */
    EMB_CHECK(embi_forms_key(e.c, t, "Home", false, false, false));
    EMB_CHECK(js_true(e, "t.selectionStart === 0 && t.value === 'ax'"));
    EMB_CHECK(embi_forms_key(e.c, t, "a", true, false, false));
    EMB_CHECK(js_true(e, "t.selectionStart === 0 && t.selectionEnd === 2"));
    /* A canceled beforeinput changes nothing. */
    EMB_CHECK(js_true(e, "t.addEventListener('beforeinput', function (x) { x.preventDefault(); }); "
                         "t.setSelectionRange(0, 0); true"));
    EMB_CHECK(!embi_forms_edit(e.c, t, "insertText", (embi_str){"Q", 1}, false));
    EMB_CHECK(js_true(e, "t.value === 'ax'"));
    /* A read-only control takes no text. */
    embi_node *ro = by_id(e, "ro");
    EMB_REQUIRE(ro != nullptr);
    EMB_CHECK(!embi_forms_edit(e.c, ro, "insertText", (embi_str){"Q", 1}, false));
    EMB_CHECK(js_true(e, "document.getElementById('ro').value === 'ro'"));
    env_free(e);
}

EMB_TEST(bind_form_composition_replaces_its_range)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"\">'; "
                         "window.ev = []; var t = document.getElementById('t'); "
                         "t.addEventListener('input', function (x) { ev.push(x.isComposing); }); true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    embi_forms_composition_start(e.c, t);
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"ni", 2}, true));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"nih", 3}, true));
    EMB_CHECK(js_true(e, "t.value === 'nih'"));
    /* The commit replaces the composition text, and the composition ends. */
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"\xE4\xBD\xA0", 3}, true));
    embi_forms_composition_end(e.c, t);
    EMB_CHECK(js_true(e, "t.value === '\\u4f60' && ev.join(',') === 'true,true,true'"));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"!", 1}, false));
    EMB_CHECK(js_true(e, "t.value === '\\u4f60!' && ev[ev.length - 1] === false"));
    env_free(e);
}

EMB_TEST(bind_form_formdata_from_form)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f><input name=a value=1>"
                         "<input type=checkbox name=c checked><input type=checkbox name=d>"
                         "<select name=s><option value=o1 selected>o1<option value=o2></select>"
                         "<textarea name=t>tx</textarea><input name=dis disabled value=z>"
                         "<input type=submit name=sb value=go><input name=\"\"  value=nameless></form>'; true"));
    EMB_CHECK(js_true(e, "var fd = new FormData(document.getElementById('f')); "
                         "fd.get('a') === '1' && fd.get('c') === 'on' && fd.get('d') === null && fd.get('s') === 'o1' && "
                         "fd.get('t') === 'tx' && fd.get('dis') === null && fd.get('sb') === null && "
                         "fd.getAll('s').length === 1"));
    EMB_CHECK(js_true(e, "var threw = false; try { new FormData(document.body); } catch (x) { threw = x instanceof TypeError; } "
                         "threw"));
    env_free(e);
}

EMB_TEST(bind_form_submit_navigates_with_query)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f action=\"/go\"><input name=q value=\"a b\">"
                         "<input type=submit id=b value=ok></form>'; true"));
    EMB_CHECK(js_true(e, "window.sub = []; var f = document.getElementById('f'); "
                         "f.addEventListener('submit', function (x) { sub.push(x.submitter ? x.submitter.id : 'none'); }); "
                         "f.requestSubmit(); sub.join(',') === 'none'"));
    EMB_CHECK_EQ_INT(g_nav, 1);
    EMB_CHECK(strcmp(g_url, "http://127.0.0.1/go?q=a+b") == 0);
    /* The submit button submits with itself as the submitter, and a canceled submit event submits nothing. */
    EMB_CHECK(js_true(e, "document.getElementById('b').click(); sub.join(',') === 'none,b'"));
    EMB_CHECK_EQ_INT(g_nav, 2);
    EMB_CHECK(js_true(e, "f.addEventListener('submit', function (x) { x.preventDefault(); }); f.requestSubmit(); true"));
    EMB_CHECK_EQ_INT(g_nav, 2);
    /* form.submit() does not fire the submit event. */
    EMB_CHECK(js_true(e, "window.before = sub.length; f.submit(); sub.length === before"));
    EMB_CHECK_EQ_INT(g_nav, 3);
    env_free(e);
}

EMB_TEST(bind_form_reset_restores_defaults)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f><input id=i value=\"d\"><input id=c type=checkbox checked>"
                         "<input type=reset id=rb></form>'; true"));
    EMB_CHECK(js_true(e, "var i = document.getElementById('i'), c = document.getElementById('c'); "
                         "i.value = 'x'; c.checked = false; document.getElementById('f').reset(); "
                         "i.value === 'd' && c.checked === true"));
    EMB_CHECK(js_true(e, "i.value = 'y'; document.getElementById('rb').click(); i.value === 'd'"));
    env_free(e);
}

EMB_TEST(bind_form_validity_and_messages)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f><input id=r required><input id=m type=email value=\"a@\">"
                         "<input id=l maxlength=3 value=\"ab\"><input id=d disabled required></form>'; true"));
    EMB_CHECK(js_true(e, "window.bad = []; ['r', 'm', 'l', 'd'].forEach(function (id) { "
                         "  document.getElementById(id).addEventListener('invalid', function () { bad.push(id); }); }); "
                         "var r = document.getElementById('r'); true"));
    EMB_CHECK(js_true(e, "r.checkValidity() === false"));
    EMB_CHECK(js_true(e, "r.validity.valueMissing === true && r.validity.valid === false"));
    EMB_CHECK(js_true(e, "r.validity.valueMissing === true"));
    EMB_CHECK(js_true(e, "typeof r.validity === 'object' && r.validity.typeMismatch === false"));
    EMB_CHECK(js_true(e, "r.validity.patternMismatch === false && r.validity.stepMismatch === false"));
    EMB_CHECK(js_true(e, "r.validity.customError === false && r.validity.tooLong === false"));
    EMB_CHECK(js_true(e, "r.required === true && r.value === ''"));
    EMB_CHECK(js_true(e, "r.validity.valid === false"));
    EMB_CHECK(js_true(e, "r.validationMessage !== ''"));
    EMB_CHECK(js_true(e, "bad.join(',') === 'r'"));
    EMB_CHECK(js_true(e, "r.value = 'x'; r.checkValidity() === true && r.validity.valueMissing === false"));
    EMB_CHECK(js_true(e, "var m = document.getElementById('m'); m.validity.typeMismatch === true && "
                         "m.checkValidity() === false; m.value = 'a@b.c'; m.checkValidity() === true"));
    /* A disabled control is barred: never invalid. */
    EMB_CHECK(js_true(e, "var d = document.getElementById('d'); d.willValidate === false && d.checkValidity() === true && "
                         "d.validity.valueMissing === false"));
    /* A user edit longer than maxlength is too long; a script value is not checked for it. */
    EMB_CHECK(js_true(e, "var l = document.getElementById('l'); l.validity.tooLong === false && l.value === 'ab'"));
    EMB_CHECK(js_true(e, "document.getElementById('f').checkValidity() === true"));
    /* A custom message makes the control invalid until it is cleared. */
    EMB_CHECK(js_true(e, "r.value = 'x'; r.setCustomValidity('nope'); r.validity.customError === true && "
                         "r.validationMessage === 'nope' && r.checkValidity() === false; "
                         "r.setCustomValidity(''); r.checkValidity() === true"));
    env_free(e);
}

EMB_TEST(bind_form_select_and_options)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<select id=s><option value=a>Alpha<option id=ob value=b selected>"
                         "<option disabled>Gamma</select>'; true"));
    EMB_CHECK(js_true(e, "var s = document.getElementById('s'); s.value === 'b' && s.selectedIndex === 1 && "
                         "s.length === 3 && s.type === 'select-one' && s.options.length === 3"));
    EMB_CHECK(js_true(e, "s.value = 'a'; s.selectedIndex === 0 && document.getElementById('ob').selected === false"));
    EMB_CHECK(js_true(e, "s.selectedIndex = 2; s.selectedIndex === 2"));
    EMB_CHECK(js_true(e, "document.getElementById('ob').selected = true; s.selectedIndex === 1 && "
                         "s.options[0].selected === false"));
    EMB_CHECK(js_true(e, "var o = s.options[2]; o.text === 'Gamma' && o.label === 'Gamma' && o.index === 2 && "
                         "o.form === null"));
    env_free(e);
}

EMB_TEST(bind_form_implicit_submission_and_textarea_enter)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f action=\"/one\"><input id=one name=q>"
                         "<textarea id=t></textarea></form><form id=g action=\"/two\"><input id=g1 name=a>"
                         "<input id=g2 name=b><input type=submit value=s></form>'; true"));
    /* A single text field submits its form when Enter is pressed (implicit submission). */
    EMB_CHECK(embi_forms_implicit_submit(e.c, by_id(e, "one")));
    EMB_CHECK_EQ_INT(g_nav, 1);
    EMB_CHECK(strcmp(g_url, "http://127.0.0.1/one?q=") == 0);
    /* Two fields with a submit button: the default button is clicked (through its own activation). */
    EMB_CHECK(embi_forms_implicit_submit(e.c, by_id(e, "g1")));
    EMB_CHECK_EQ_INT(g_nav, 2);
    EMB_CHECK(strcmp(g_url, "http://127.0.0.1/two?a=&b=") == 0);
    /* A textarea takes no implicit submission. */
    EMB_CHECK(!embi_forms_implicit_submit(e.c, by_id(e, "t")));
    env_free(e);
}

/* ---- W5e part 2: change order, composition end states, history, pattern/step, POST body, painter state ---- */

EMB_TEST(bind_form_change_fires_on_commit_after_input_and_not_without_an_edit)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"ab\"><input id=o>'; true"));
    EMB_CHECK(js_true(e, "window.log = []; var t = document.getElementById('t'); "
                         "t.addEventListener('input', function () { log.push('input'); });"
                         "t.addEventListener('change', function () { log.push('change'); }); true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    embi_forms_commit(e.c, t); /* no edit yet: nothing */
    EMB_CHECK(js_true(e, "log.length === 0"));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"x", 1}, false));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"y", 1}, false));
    EMB_CHECK(js_true(e, "log.join() === 'input,input'"));
    embi_forms_commit(e.c, t);
    EMB_CHECK(js_true(e, "log.join() === 'input,input,change'"));
    embi_forms_commit(e.c, t); /* once per edit run */
    EMB_CHECK(js_true(e, "log.length === 3"));
    /* blur commits too */
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"z", 1}, false));
    embi_forms_blur(e.c, t);
    EMB_CHECK(js_true(e, "log[log.length - 1] === 'change' && log.length === 5"));
    env_free(e);
}

EMB_TEST(bind_form_composition_commit_and_cancel)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"ab\">'; true"));
    EMB_CHECK(js_true(e, "window.log = []; var t = document.getElementById('t'); t.setSelectionRange(2, 2); "
                         "t.addEventListener('input', function (x) { log.push(x.inputType + ':' + x.isComposing); }); true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    embi_forms_composition_start(e.c, t);
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"ni", 2}, true));
    EMB_CHECK(js_true(e, "t.value === 'abni'"));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"\xE4\xBD\xA0", 3}, true));
    EMB_CHECK(js_true(e, "t.value === 'ab\\u4f60'"));
    embi_forms_composition_end(e.c, t);
    EMB_CHECK(js_true(e, "t.value === 'ab\\u4f60' && log[0] === 'insertCompositionText:true'"));
    /* a cancelled composition (an empty update) leaves the original text */
    embi_forms_composition_start(e.c, t);
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"q", 1}, true));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertCompositionText", (embi_str){"", 0}, true));
    embi_forms_composition_end(e.c, t);
    EMB_CHECK(js_true(e, "t.value === 'ab\\u4f60'"));
    env_free(e);
}

EMB_TEST(bind_form_undo_and_redo_restore_values_and_fire_history_inputs)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"\">'; true"));
    EMB_CHECK(js_true(e, "window.log = []; var t = document.getElementById('t'); "
                         "t.addEventListener('input', function (x) { log.push(x.inputType); }); true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"a", 1}, false));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"b", 1}, false));
    EMB_CHECK(js_true(e, "t.value === 'ab'"));
    EMB_CHECK(embi_forms_undo(e.c, t, false));
    EMB_CHECK(js_true(e, "t.value === 'a'"));
    EMB_CHECK(embi_forms_undo(e.c, t, false));
    EMB_CHECK(js_true(e, "t.value === ''"));
    EMB_CHECK(!embi_forms_undo(e.c, t, false));
    EMB_CHECK(embi_forms_undo(e.c, t, true));
    EMB_CHECK(js_true(e, "t.value === 'a'"));
    EMB_CHECK(embi_forms_key(e.c, t, "z", true, false, false));
    EMB_CHECK(js_true(e, "t.value === ''"));
    EMB_CHECK(embi_forms_key(e.c, t, "y", true, false, false));
    EMB_CHECK(js_true(e, "t.value === 'a'"));
    /* an edit after an undo drops the redo stack */
    EMB_CHECK(embi_forms_key(e.c, t, "z", true, false, false));
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"q", 1}, false));
    EMB_CHECK(!embi_forms_undo(e.c, t, true));
    EMB_CHECK(js_true(e, "t.value === 'q' && log.indexOf('historyUndo') >= 0 && log.indexOf('historyRedo') >= 0"));
    env_free(e);
}

EMB_TEST(bind_form_pattern_and_step_constraints)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=p pattern=\"[a-c]{2}[0-9]+\" value=\"ab12\">"
                         "<input id=n type=number step=2 min=0 value=\"3\"><input id=m type=number step=0.5 value=\"1.5\">'; true"));
    EMB_CHECK(js_true(e, "var p = document.getElementById('p'); p.validity.patternMismatch === false && p.checkValidity()"));
    EMB_CHECK(js_true(e, "p.value = 'abc'; p.validity.patternMismatch === true && p.validity.valid === false"));
    EMB_CHECK(js_true(e, "p.value = 'ab9'; p.validity.patternMismatch === false"));
    EMB_CHECK(js_true(e, "p.value = 'xab9'; p.validity.patternMismatch === true"));
    EMB_CHECK(js_true(e, "var n = document.getElementById('n'); n.validity.stepMismatch === true && n.validity.valid === false"));
    EMB_CHECK(js_true(e, "n.value = '4'; n.validity.stepMismatch === false"));
    EMB_CHECK(js_true(e, "var m = document.getElementById('m'); m.validity.stepMismatch === false"));
    EMB_CHECK(js_true(e, "n.value = '4'; n.stepUp(); n.value === '6'"));
    EMB_CHECK(js_true(e, "n.stepDown(2); n.value === '2'"));
    EMB_CHECK(js_true(e, "n.valueAsNumber === 2"));
    env_free(e);
}

/* The navigate_form hook of the tests: remembers the method, the body and the content type. */
static char g_method[16];
static char g_body[1024];
static char g_ctype[160];
static char g_form_url[512];
static int g_nav_form;

static bool capture_form(void *user, embi_str method, embi_str url, embi_str body, embi_str ctype)
{
    (void)user;
    snprintf(g_form_url, sizeof g_form_url, "%.*s", (int)url.len, url.ptr);
    g_nav_form++;
    snprintf(g_method, sizeof g_method, "%.*s", (int)method.len, method.ptr);
    snprintf(g_body, sizeof g_body, "%.*s", (int)body.len, body.ptr);
    snprintf(g_ctype, sizeof g_ctype, "%.*s", (int)ctype.len, ctype.ptr);
    return true;
}

EMB_TEST(bind_form_post_submission_carries_a_body_and_content_type)
{
    env e = env_new();
    embi_bind_host h;
    memset(&h, 0, sizeof h);
    h.navigate = capture_nav;
    h.navigate_form = capture_form;
    h.url = url_of;
    embi_bind_set_host(e.c, &h);
    g_nav_form = 0;
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f method=post action=\"/s\"><input name=a value=\"x y\">"
                         "<input name=b value=\"&\"><input type=submit id=s></form>'; true"));
    EMB_CHECK(js_true(e, "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK_EQ_INT(g_nav_form, 1);
    EMB_CHECK(strcmp(g_method, "POST") == 0);
    EMB_CHECK(strcmp(g_body, "a=x+y&b=%26") == 0);
    EMB_CHECK(strcmp(g_ctype, "application/x-www-form-urlencoded") == 0);
    EMB_CHECK(js_true(e, "document.getElementById('f').enctype = 'multipart/form-data'; "
                         "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK_EQ_INT(g_nav_form, 2);
    EMB_CHECK(strncmp(g_ctype, "multipart/form-data; boundary=", 30) == 0);
    EMB_CHECK(strstr(g_body, "Content-Disposition: form-data; name=\"a\"\r\n\r\nx y\r\n") != nullptr);
    EMB_CHECK(js_true(e, "document.getElementById('f').enctype = 'text/plain'; "
                         "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK(strcmp(g_body, "a=x y\r\nb=&\r\n") == 0);
    /* a GET form still navigates with a query */
    EMB_CHECK(js_true(e, "document.getElementById('f').method = 'get'; document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK(strstr(g_form_url, "?a=x+y&b=%26") != nullptr);
    env_free(e);
}

EMB_TEST(bind_form_formaction_of_the_submitter_overrides_the_form)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f action=\"/one\"><input name=a value=1>"
                         "<button id=b formaction=\"/two\">go</button></form>'; true"));
    EMB_CHECK(js_true(e, "document.getElementById('b').click(); true"));
    EMB_CHECK_EQ_INT(g_nav, 1);
    EMB_CHECK(strstr(g_url, "/two?a=1") != nullptr);
    env_free(e);
}

EMB_TEST(bind_form_paint_state_reports_value_selection_and_focus)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"hello\" placeholder=\"ph\">"
                         "<input id=c type=checkbox checked><select id=s><option>a<option selected>b</select>"
                         "<input id=r type=range min=0 max=10 value=5>'; true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(js_true(e, "document.getElementById('t').setSelectionRange(1, 3); true"));
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    EMB_CHECK(embi_forms_paint_state(t, &st));
    EMB_CHECK_EQ_INT(st.kind, EMBI_PCTL_TEXT);
    EMB_CHECK(st.text_len == 5 && memcmp(st.text, "hello", 5) == 0);
    EMB_CHECK(st.sel_a == 1 && st.sel_b == 3 && !st.focused);
    embi_forms_focus(e.c, t);
    EMB_CHECK(embi_forms_paint_state(t, &st));
    EMB_CHECK(st.focused);
    EMB_CHECK(embi_forms_paint_state(by_id(e, "c"), &st) && st.kind == EMBI_PCTL_CHECKBOX && st.checked);
    embi_forms_focus(e.c, by_id(e, "s")); /* the state block of a select exists from its first focus */
    EMB_CHECK(embi_forms_paint_state(by_id(e, "s"), &st) && st.kind == EMBI_PCTL_SELECT && st.nopts == 2);
    EMB_CHECK(st.text_len == 1 && st.text[0] == 'b');
    EMB_CHECK(embi_forms_paint_state(by_id(e, "r"), &st) && st.kind == EMBI_PCTL_RANGE && st.value == 5.0 && st.max == 10.0);
    uint32_t g0 = embi_forms_generation();
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"!", 1}, false));
    EMB_CHECK(embi_forms_generation() != g0);
    env_free(e);
}

EMB_TEST(bind_form_controls_release_their_memory)
{
    for (int i = 0; i < 2; i++) {
        env w = env_new();
        EMB_CHECK(js_true(w, "document.body.innerHTML = '<input id=t value=\"abc\"><textarea id=a>x</textarea>"
                             "<select id=s><option>q</select>'; true"));
        embi_node *t = by_id(w, "t");
        EMB_REQUIRE(t != nullptr);
        EMB_CHECK(embi_forms_edit(w.c, t, "insertText", (embi_str){"zz", 2}, false));
        env_free(w);
    }
    size_t before = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=t value=\"abc\"><textarea id=a>x</textarea>"
                         "<select id=s><option>q</select>'; true"));
    embi_node *t = by_id(e, "t");
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK(embi_forms_edit(e.c, t, "insertText", (embi_str){"zz", 2}, false));
    EMB_CHECK(embi_forms_undo(e.c, t, false));
    env_free(e);
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(bind_form_pattern_does_not_overflow_the_stack_on_long_values)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=p>'; window.p = document.getElementById('p'); "
                         "window.big = 'a'.repeat(100000); true"));
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '.*'); p.value = big; p.validity.patternMismatch === false"));
    /* group repeats beyond the depth cap count as "do not constrain" */
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '(a|b)*'); p.value = big; p.validity.patternMismatch === false"));
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '[a-z]*'); p.value = big; p.validity.patternMismatch === false"));
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '[a-z]*x'); p.value = big; p.validity.patternMismatch === true"));
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '(?:a|b)+'); p.value = 'ab'.repeat(10); p.validity.patternMismatch === false"));
    EMB_CHECK(js_true(e, "p.value = 'abc'; p.validity.patternMismatch === true"));
    env_free(e);
}

EMB_TEST(bind_form_pattern_matches_multibyte_values_by_character)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=p>'; window.p = document.getElementById('p'); true"));
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '.?[\u00a9]'); p.value = '\u00e9'; p.validity.patternMismatch === true"));
    EMB_CHECK(js_true(e, "p.value = '\u00e9\u00a9'; p.validity.patternMismatch === false"));
    EMB_CHECK(js_true(e, "p.setAttribute('pattern', '.*\u00e9'); p.value = 'a\u00e9\u00e9'; p.validity.patternMismatch === false"));
    env_free(e);
}

EMB_TEST(bind_form_report_validity_logs_the_message)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f><input id=a required><input id=b></form>'; true"));
    EMB_CHECK(js_true(e, "var a = document.getElementById('a'); a.reportValidity() === false"));
    EMB_CHECK(strstr(g_console, "Please fill out this field.|") != nullptr);
    g_console[0] = '\0';
    EMB_CHECK(js_true(e, "document.getElementById('b').reportValidity() === true"));
    EMB_CHECK(g_console[0] == '\0');
    /* checkValidity stays silent */
    EMB_CHECK(js_true(e, "a.checkValidity() === false"));
    EMB_CHECK(g_console[0] == '\0');
    /* a canceled invalid event suppresses the report; the form reports every invalid control */
    EMB_CHECK(js_true(e, "a.addEventListener('invalid', function (x) { x.preventDefault(); }); a.reportValidity() === false"));
    EMB_CHECK(g_console[0] == '\0');
    EMB_CHECK(js_true(e, "a.setCustomValidity('nope'); var f = document.getElementById('f'); "
                         "f.reportValidity() === false"));
    EMB_CHECK(g_console[0] == '\0');
    EMB_CHECK(js_true(e, "var t = document.createElement('input'); t.required = true; f.appendChild(t); "
                         "f.reportValidity() === false"));
    EMB_CHECK(strstr(g_console, "Please fill out this field.|") != nullptr);
    env_free(e);
}

EMB_TEST(bind_form_stateless_controls_paint_through_the_attribute_fallback)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=i placeholder=name><select id=s><option>a<option selected>b</select>'; true"));
    embi_paint_ctl st;
    /* no script, focus or edit touched them: there is no state block, so the painter reads the attributes itself */
    EMB_CHECK(!embi_forms_paint_state(by_id(e, "i"), &st));
    EMB_CHECK(!embi_forms_paint_state(by_id(e, "s"), &st));
    size_t before = embi_mem_live();
    EMB_CHECK(!embi_forms_paint_state(by_id(e, "i"), &st));
    EMB_CHECK(embi_mem_live() == before);
    /* once the control has a state block, the placeholder is filled in */
    EMB_CHECK(js_true(e, "document.getElementById('i').setSelectionRange(0, 0); true"));
    EMB_CHECK(embi_forms_paint_state(by_id(e, "i"), &st));
    EMB_CHECK(st.placeholder != nullptr && st.placeholder_len == 4 && memcmp(st.placeholder, "name", 4) == 0);
    env_free(e);
}

EMB_TEST(bind_form_submission_normalizes_line_breaks_and_resolves_get_urls)
{
    env e = env_new();
    embi_bind_host h;
    memset(&h, 0, sizeof h);
    h.navigate = capture_nav;
    h.navigate_form = capture_form;
    h.url = url_of;
    embi_bind_set_host(e.c, &h);
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<form id=f method=post action=\"/s\"><textarea name=t id=t></textarea>"
                         "<input type=submit></form>'; document.getElementById('t').value = 'a\\nb'; true"));
    EMB_CHECK(js_true(e, "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK(strcmp(g_body, "t=a%0D%0Ab") == 0);
    EMB_CHECK(js_true(e, "new FormData(document.getElementById('f')).get('t') === 'a\\nb'"));
    EMB_CHECK(js_true(e, "document.getElementById('f').enctype = 'multipart/form-data'; "
                         "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK(strstr(g_body, "name=\"t\"\r\n\r\na\r\nb\r\n") != nullptr);
    EMB_CHECK(js_true(e, "document.getElementById('f').enctype = 'text/plain'; "
                         "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK(strcmp(g_body, "t=a\r\nb\r\n") == 0);
    /* a boundary inside a value is never used */
    EMB_CHECK(js_true(e, "document.getElementById('t').value = '----EmberFormBoundary'; "
                         "document.getElementById('f').enctype = 'multipart/form-data'; "
                         "document.getElementById('f').requestSubmit(); true"));
    EMB_CHECK(strncmp(g_ctype, "multipart/form-data; boundary=", 30) == 0);
    EMB_CHECK(strstr(g_body, g_ctype + 30) == g_body + 2);
    /* GET: an existing query is replaced and the fragment stays after the entries */
    EMB_CHECK(js_true(e, "var f = document.getElementById('f'); f.method = 'get'; f.enctype = ''; "
                         "document.getElementById('t').value = 'x'; f.action = '/search?lang=en'; f.requestSubmit(); true"));
    EMB_CHECK(strstr(g_form_url, "/search?t=x") != nullptr);
    EMB_CHECK(strstr(g_form_url, "lang=en") == nullptr);
    EMB_CHECK(js_true(e, "f.action = '/p#frag'; f.requestSubmit(); true"));
    EMB_CHECK(strstr(g_form_url, "/p?t=x#frag") != nullptr);
    EMB_CHECK(js_true(e, "f.action = '/q?a=1#z'; f.requestSubmit(); true"));
    EMB_CHECK(strstr(g_form_url, "/q?t=x#z") != nullptr);
    env_free(e);
}

/* A number field is edited as a text field (Chromium): typed text keeps only the number characters, the field shows what was
 * typed while the value is the empty string until the text is a valid number (badInput, valueAsNumber NaN), the selection
 * API reads null and throws, and select() and Ctrl+A select the text that the next edit replaces. */
EMB_TEST(bind_form_number_field_edits_as_text)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=n type=number value=\"16\"><input id=m type=email>'; true"));
    EMB_CHECK(js_true(e, "window.log = []; var n = document.getElementById('n'); "
                         "n.addEventListener('beforeinput', function (x) { log.push('before:' + x.data); });"
                         "n.addEventListener('input', function (x) { log.push('input:' + n.value); }); true"));
    embi_node *n = by_id(e, "n");
    EMB_REQUIRE(n != nullptr);
    /* the selection API does not apply to number (nor to email) */
    EMB_CHECK(js_true(e, "n.selectionStart === null && n.selectionEnd === null && n.selectionDirection === null"));
    EMB_CHECK(js_true(e, "var m = document.getElementById('m'); m.selectionStart === null && m.selectionEnd === null"));
    EMB_CHECK(js_true(e, "var threw = ''; try { n.setSelectionRange(0, 1); } catch (x) { threw = x.name; } "
                         "threw === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "threw = ''; try { n.selectionStart = 0; } catch (x) { threw = x.name; } threw === 'InvalidStateError'"));
    EMB_CHECK(js_true(e, "threw = ''; try { n.selectionDirection = 'forward'; } catch (x) { threw = x.name; } "
                         "threw === 'InvalidStateError'"));
    /* Ctrl+A selects the text, typing replaces it */
    EMB_CHECK(embi_forms_key(e.c, n, "a", true, false, false));
    EMB_CHECK(embi_forms_edit(e.c, n, "insertText", (embi_str){"12", 2}, false));
    EMB_CHECK(js_true(e, "n.value === '12' && n.valueAsNumber === 12"));
    EMB_CHECK(js_true(e, "log.join() === 'before:12,input:12'"));
    /* characters that are not part of a number are dropped; text with none of them is no edit at all */
    EMB_CHECK(js_true(e, "log = []; true"));
    EMB_CHECK(!embi_forms_edit(e.c, n, "insertText", (embi_str){"x", 1}, false));
    EMB_CHECK(js_true(e, "log.length === 0 && n.value === '12'"));
    EMB_CHECK(embi_forms_edit(e.c, n, "insertText", (embi_str){"a.5", 3}, false));
    EMB_CHECK(js_true(e, "n.value === '12.5' && log.join() === 'before:.5,input:12.5'"));
    /* text that is not (yet) a number: the value is empty, the field shows the text */
    EMB_CHECK(embi_forms_edit(e.c, n, "insertText", (embi_str){"e", 1}, false));
    EMB_CHECK(js_true(e, "n.value === '' && isNaN(n.valueAsNumber) && n.validity.badInput && !n.validity.valid"));
    embi_paint_ctl st;
    memset(&st, 0, sizeof st);
    EMB_CHECK(embi_forms_paint_state(n, &st));
    EMB_CHECK(st.kind == EMBI_PCTL_TEXT && st.text_len == 5 && memcmp(st.text, "12.5e", 5) == 0 && st.caret == 5);
    EMB_CHECK(embi_forms_edit(e.c, n, "insertText", (embi_str){"2", 1}, false));
    EMB_CHECK(js_true(e, "n.value === '12.5e2' && n.valueAsNumber === 1250 && !n.validity.badInput"));
    /* Backspace edits as in a text field */
    EMB_CHECK(embi_forms_key(e.c, n, "Backspace", false, false, false));
    EMB_CHECK(embi_forms_key(e.c, n, "Backspace", false, false, false));
    EMB_CHECK(js_true(e, "n.value === '12.5'"));
    /* select() selects the text: the next edit replaces it */
    EMB_CHECK(js_true(e, "n.select(); true"));
    EMB_CHECK(embi_forms_edit(e.c, n, "insertText", (embi_str){"7", 1}, false));
    EMB_CHECK(js_true(e, "n.value === '7'"));
    /* the value setter sanitizes */
    EMB_CHECK(js_true(e, "n.value = 'abc'; n.value === '' && !n.validity.badInput"));
    EMB_CHECK(js_true(e, "n.value = '-1.5e1'; n.value === '-1.5e1' && n.valueAsNumber === -15"));
    /* badInput is reported before valueMissing */
    EMB_CHECK(js_true(e, "n.required = true; n.value = ''; n.validationMessage === 'Please fill out this field.'"));
    EMB_CHECK(embi_forms_edit(e.c, n, "insertText", (embi_str){"e", 1}, false));
    EMB_CHECK(js_true(e, "n.validationMessage === 'Please enter a number.'"));
    /* an email field's value has no white space around the typed text, which the field still shows */
    embi_node *m = by_id(e, "m");
    EMB_REQUIRE(m != nullptr);
    EMB_CHECK(embi_forms_edit(e.c, m, "insertText", (embi_str){" a@b.c ", 7}, false));
    EMB_CHECK(js_true(e, "m.value === 'a@b.c' && m.validity.valid"));
    embi_paint_ctl mp;
    memset(&mp, 0, sizeof mp);
    EMB_CHECK(embi_forms_paint_state(m, &mp));
    EMB_CHECK(mp.text_len == 7 && mp.caret == 7);
    env_free(e);
}

/* ArrowUp and ArrowDown step a number field as Chromium's spin button does, firing input and then change on each step: by the
 * step (1 for step=any) within min and max, snapping a value off the step, without the rounding error of binary fractions. */
EMB_TEST(bind_form_number_field_arrow_keys_step)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "document.body.innerHTML = '<input id=a type=number min=0 max=10 step=2 value=4>"
                         "<input id=b type=number step=any value=1.5><input id=c type=number min=5>"
                         "<input id=d type=number step=0.1 value=0.1><input id=r type=number value=3 readonly>'; true"));
    EMB_CHECK(js_true(e, "window.log = []; ['a','b','c','d'].forEach(function (id) { var x = document.getElementById(id); "
                         "x.addEventListener('input', function () { log.push('input:' + x.value); }); "
                         "x.addEventListener('change', function () { log.push('change:' + x.value); }); }); true"));
    embi_node *a = by_id(e, "a");
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(embi_forms_key(e.c, a, "ArrowUp", false, false, false));
    EMB_CHECK(js_true(e, "log.join() === 'input:6,change:6'"));
    EMB_CHECK(embi_forms_key(e.c, a, "ArrowUp", false, false, false));
    EMB_CHECK(embi_forms_key(e.c, a, "ArrowUp", false, false, false));
    EMB_CHECK(embi_forms_key(e.c, a, "ArrowUp", false, false, false)); /* at max: no step, no event */
    EMB_CHECK(js_true(e, "log.join() === 'input:6,change:6,input:8,change:8,input:10,change:10'"));
    EMB_CHECK(js_true(e, "log = []; document.getElementById('a').value = '5'; true"));
    EMB_CHECK(embi_forms_key(e.c, a, "ArrowDown", false, false, false)); /* off the step: snaps down */
    EMB_CHECK(js_true(e, "log.join() === 'input:4,change:4'"));
    EMB_CHECK(js_true(e, "log = []; true"));
    EMB_CHECK(embi_forms_key(e.c, by_id(e, "b"), "ArrowUp", false, false, false));
    EMB_CHECK(js_true(e, "document.getElementById('b').value === '2.5'"));
    EMB_CHECK(embi_forms_key(e.c, by_id(e, "c"), "ArrowUp", false, false, false)); /* empty: starts at min */
    EMB_CHECK(js_true(e, "document.getElementById('c').value === '5'"));
    EMB_CHECK(embi_forms_key(e.c, by_id(e, "d"), "ArrowUp", false, false, false));
    EMB_CHECK(embi_forms_key(e.c, by_id(e, "d"), "ArrowUp", false, false, false));
    EMB_CHECK(js_true(e, "document.getElementById('d').value === '0.3'"));
    EMB_CHECK(embi_forms_key(e.c, by_id(e, "r"), "ArrowUp", false, false, false)); /* read-only: taken, unchanged */
    EMB_CHECK(js_true(e, "document.getElementById('r').value === '3'"));
    env_free(e);
}

EMB_TEST_MAIN()
