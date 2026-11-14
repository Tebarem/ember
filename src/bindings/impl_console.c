/*
 * The console namespace (console.webidl). Output goes through embi_bind_log, which calls the
 * embedder's log hook and embi_log_*. Arguments are formatted like the browsers do: a leading
 * string with % directives consumes the following arguments, the rest are inspected and joined
 * with spaces.
 */
#include "bindings/impl.h"
#include "bindings/gen/console.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/string.h"

enum { INSPECT_DEPTH = 2, INSPECT_MAX_KEYS = 20, INSPECT_MAX_ITEMS = 100 };

static void append_jsstr(embi_bind_ctx *c, embi_sb *sb, const embi_js_string *s)
{
    (void)c;
    if (s) {
        embi_js_str_append_utf8(s, sb);
    }
}

/* ToString for primitives; "" and a cleared exception when it throws. */
static void append_to_string(embi_bind_ctx *c, embi_sb *sb, embi_js_value v)
{
    embi_js_string *s = embi_js_to_string(c->rt, v);
    if (!s) {
        embi_js_exception_clear(c->rt);
        embi_sb_append(sb, EMBI_STR("[unprintable]"));
        return;
    }
    append_jsstr(c, sb, s);
}

static void inspect(embi_bind_ctx *c, embi_js_value v, int depth, embi_sb *sb, bool nested)
{
    embi_js_runtime *rt = c->rt;
    if (embi_js_is_string(v)) {
        if (nested) {
            embi_sb_append_char(sb, '\'');
            append_jsstr(c, sb, embi_js_as_string(v));
            embi_sb_append_char(sb, '\'');
        } else {
            append_jsstr(c, sb, embi_js_as_string(v));
        }
        return;
    }
    if (embi_js_is_symbol(v)) {
        embi_sb_append(sb, EMBI_STR("Symbol()"));
        return;
    }
    if (!embi_js_is_object(v)) {
        append_to_string(c, sb, v);
        if (embi_js_is_bigint(v)) {
            embi_sb_append_char(sb, 'n');
        }
        return;
    }
    embi_js_object *o = embi_js_as_object(v);
    if (embi_js_is_callable(v)) {
        embi_js_value nm = embi_js_obj_get(rt, o, embi_js_key_c(rt, "name"), v);
        embi_sb_append(sb, EMBI_STR("[Function"));
        if (nm != EMBI_JS_EXCEPTION && embi_js_is_string(nm) && embi_js_str_len(embi_js_as_string(nm))) {
            embi_sb_append(sb, EMBI_STR(": "));
            append_jsstr(c, sb, embi_js_as_string(nm));
        } else if (nm == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
        }
        embi_sb_append_char(sb, ']');
        return;
    }
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo) {
        embi_sb_append(sb, EMBI_STR("["));
        embi_sb_append(sb, (embi_str){embi_bind_names[bo->iface], (uint32_t)SDL_strlen(embi_bind_names[bo->iface])});
        embi_sb_append(sb, EMBI_STR("]"));
        return;
    }
    if (o->klass == &embi_js_class_error) {
        embi_js_value st = embi_js_obj_get(rt, o, embi_js_key_c(rt, "stack"), v);
        if (st != EMBI_JS_EXCEPTION && embi_js_is_string(st)) {
            append_jsstr(c, sb, embi_js_as_string(st));
            return;
        }
        if (st == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
        }
        embi_js_value nm = embi_js_obj_get(rt, o, embi_js_key_c(rt, "name"), v);
        embi_js_value msg = embi_js_obj_get(rt, o, embi_js_key_c(rt, "message"), v);
        if (nm == EMBI_JS_EXCEPTION || msg == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
            embi_sb_append(sb, EMBI_STR("Error"));
            return;
        }
        append_to_string(c, sb, nm);
        embi_sb_append(sb, EMBI_STR(": "));
        append_to_string(c, sb, msg);
        return;
    }
    bool is_arr = embi_js_is_array(v);
    if (depth >= INSPECT_DEPTH) {
        embi_sb_append(sb, is_arr ? EMBI_STR("[Array]") : EMBI_STR("[Object]"));
        return;
    }
    if (is_arr) {
        embi_js_value lenv = embi_js_obj_get(rt, o, embi_js_key_c(rt, "length"), v);
        double len = 0;
        if (lenv == EMBI_JS_EXCEPTION || !embi_js_to_number(rt, lenv, &len)) {
            embi_js_exception_clear(rt);
            len = 0;
        }
        embi_sb_append(sb, EMBI_STR("["));
        uint32_t n = len > INSPECT_MAX_ITEMS ? INSPECT_MAX_ITEMS : (uint32_t)len;
        for (uint32_t i = 0; i < n; i++) {
            embi_sb_append(sb, i ? EMBI_STR(", ") : EMBI_STR(" "));
            embi_js_value e = embi_js_obj_get(rt, o, embi_js_key_from_index(i), v);
            if (e == EMBI_JS_EXCEPTION) {
                embi_js_exception_clear(rt);
                embi_sb_append(sb, EMBI_STR("<error>"));
            } else {
                inspect(c, e, depth + 1, sb, true);
            }
        }
        if (len > n) {
            embi_sb_append(sb, EMBI_STR(", ..."));
        }
        embi_sb_append(sb, n ? EMBI_STR(" ]") : EMBI_STR("]"));
        return;
    }
    embi_js_key_vec ks = {0};
    if (embi_js_enumerable_own_keys(rt, o, &ks) != EMBI_JS_YES) {
        embi_js_exception_clear(rt);
        embi_vec_free(&ks);
        embi_sb_append(sb, EMBI_STR("{}"));
        return;
    }
    embi_sb_append(sb, EMBI_STR("{"));
    uint32_t shown = 0;
    for (uint32_t i = 0; i < ks.len && shown < INSPECT_MAX_KEYS; i++) {
        embi_js_pdesc d;
        if (embi_js_obj_get_own_property(rt, o, ks.data[i], &d) != EMBI_JS_YES) {
            embi_js_exception_clear(rt);
            continue;
        }
        embi_sb_append(sb, shown ? EMBI_STR(", ") : EMBI_STR(" "));
        shown++;
        embi_str key = embi_bind_key_utf8(c, ks.data[i]);
        embi_sb_append(sb, key);
        embi_sb_append(sb, EMBI_STR(": "));
        if (d.has & (EMBI_JS_PD_GET | EMBI_JS_PD_SET)) {
            embi_sb_append(sb, EMBI_STR("[Getter/Setter]"));
        } else {
            inspect(c, d.value, depth + 1, sb, true);
        }
    }
    if (ks.len > shown) {
        embi_sb_append(sb, EMBI_STR(", ..."));
    }
    embi_sb_append(sb, shown ? EMBI_STR(" }") : EMBI_STR("}"));
    embi_vec_free(&ks);
}

/* Formats the arguments into sb. */
static void format_args(embi_bind_ctx *c, int n, const embi_js_value *a, embi_sb *sb)
{
    int next = 0;
    if (n > 0 && embi_js_is_string(a[0])) {
        embi_sb fmt = {0};
        append_jsstr(c, &fmt, embi_js_as_string(a[0]));
        next = 1;
        for (uint32_t i = 0; i < fmt.len; i++) {
            char ch = fmt.data[i];
            if (ch != '%' || i + 1 >= fmt.len) {
                embi_sb_append_char(sb, ch);
                continue;
            }
            char d = fmt.data[i + 1];
            if (d == '%') {
                embi_sb_append_char(sb, '%');
                i++;
            } else if ((d == 's' || d == 'd' || d == 'i' || d == 'f' || d == 'o' || d == 'O' || d == 'c') &&
                       next < n) {
                embi_js_value v = a[next++];
                i++;
                if (d == 's') {
                    inspect(c, v, INSPECT_DEPTH, sb, false);
                } else if (d == 'd' || d == 'i' || d == 'f') {
                    double num = 0;
                    if (!embi_js_is_object(v) && !embi_js_is_symbol(v) && embi_js_to_number(c->rt, v, &num)) {
                        if (d == 'f') {
                            append_to_string(c, sb, embi_js_number(num));
                        } else {
                            append_to_string(c, sb, embi_js_number(num < 0 ? -(double)(int64_t)(-num) : (double)(int64_t)num));
                        }
                    } else {
                        embi_js_exception_clear(c->rt);
                        embi_sb_append(sb, EMBI_STR("NaN"));
                    }
                } else if (d == 'c') {
                    /* CSS styling: consumed, not printed */
                } else {
                    inspect(c, v, 0, sb, true);
                }
            } else {
                embi_sb_append_char(sb, ch);
            }
        }
        embi_sb_free(&fmt);
    }
    for (int i = next; i < n; i++) {
        if (sb->len) {
            embi_sb_append_char(sb, ' ');
        }
        inspect(c, a[i], 0, sb, false);
    }
}

static void emit(embi_bind_ctx *c, int level, int n, const embi_js_value *a, const char *prefix)
{
    embi_sb sb = {0};
    for (uint32_t i = 0; i < c->group_depth; i++) {
        embi_sb_append(&sb, EMBI_STR("  "));
    }
    if (prefix) {
        embi_sb_append(&sb, (embi_str){prefix, (uint32_t)SDL_strlen(prefix)});
    }
    format_args(c, n, a, &sb);
    embi_bind_log(c, level, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
}

#define LOG_FN(NAME, LEVEL, PREFIX)                                                                    \
    void embi_impl_console_##NAME(embi_bind_ctx *c, int n, const embi_js_value *a)                     \
    {                                                                                                  \
        emit(c, LEVEL, n, a, PREFIX);                                                                  \
    }

LOG_FN(log, EMBI_BIND_LOG_INFO, nullptr)
LOG_FN(info, EMBI_BIND_LOG_INFO, nullptr)
LOG_FN(debug, EMBI_BIND_LOG_DEBUG, nullptr)
LOG_FN(warn, EMBI_BIND_LOG_WARN, nullptr)
LOG_FN(error, EMBI_BIND_LOG_ERROR, nullptr)
LOG_FN(dirxml, EMBI_BIND_LOG_INFO, nullptr)

void embi_impl_console_assert(embi_bind_ctx *c, bool a0, int n, const embi_js_value *a)
{
    if (a0) {
        return;
    }
    embi_sb sb = {0};
    embi_sb_append(&sb, EMBI_STR("Assertion failed"));
    if (n > 0) {
        embi_sb_append(&sb, EMBI_STR(": "));
        format_args(c, n, a, &sb);
    }
    for (uint32_t i = 0; i < c->group_depth; i++) {
        embi_sb tmp = {0};
        embi_sb_append(&tmp, EMBI_STR("  "));
        embi_sb_append(&tmp, (embi_str){sb.data, sb.len});
        embi_sb_free(&sb);
        sb = tmp;
    }
    embi_bind_log(c, EMBI_BIND_LOG_ERROR, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
}

void embi_impl_console_clear(embi_bind_ctx *c)
{
    c->group_depth = 0;
}

void embi_impl_console_table(embi_bind_ctx *c, embi_js_value a0, embi_bind_seq a1)
{
    (void)a1;
    emit(c, EMBI_BIND_LOG_INFO, 1, &a0, nullptr);
}

void embi_impl_console_trace(embi_bind_ctx *c, int n, const embi_js_value *a)
{
    emit(c, EMBI_BIND_LOG_INFO, n, a, "Trace: ");
}

void embi_impl_console_dir(embi_bind_ctx *c, embi_js_value a0, embi_js_value a1)
{
    (void)a1;
    embi_sb sb = {0};
    inspect(c, a0, 0, &sb, true);
    embi_bind_log(c, EMBI_BIND_LOG_INFO, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
}

static void label_line(embi_bind_ctx *c, int level, embi_str label, const char *suffix_fmt, double num,
                       int n, const embi_js_value *a)
{
    embi_sb sb = {0};
    for (uint32_t i = 0; i < c->group_depth; i++) {
        embi_sb_append(&sb, EMBI_STR("  "));
    }
    embi_sb_append(&sb, label);
    char buf[64];
    int len = SDL_snprintf(buf, sizeof buf, suffix_fmt, num);
    embi_sb_append(&sb, (embi_str){buf, (uint32_t)len});
    if (n > 0) {
        embi_sb_append_char(&sb, ' ');
        format_args(c, n, a, &sb);
    }
    embi_bind_log(c, level, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
}

void embi_impl_console_count(embi_bind_ctx *c, embi_dstr a0)
{
    embi_str label = embi_bind_utf8(c, a0);
    uint32_t n = embi_bind_console_count(c, label);
    label_line(c, EMBI_BIND_LOG_INFO, label, ": %.0f", (double)n, 0, nullptr);
}

void embi_impl_console_countReset(embi_bind_ctx *c, embi_dstr a0)
{
    embi_str label = embi_bind_utf8(c, a0);
    if (!embi_bind_console_count_reset(c, label)) {
        embi_sb sb = {0};
        embi_sb_append(&sb, EMBI_STR("Count for '"));
        embi_sb_append(&sb, label);
        embi_sb_append(&sb, EMBI_STR("' does not exist"));
        embi_bind_log(c, EMBI_BIND_LOG_WARN, (embi_str){sb.data, sb.len});
        embi_sb_free(&sb);
    }
}

void embi_impl_console_group(embi_bind_ctx *c, int n, const embi_js_value *a)
{
    if (n > 0) {
        emit(c, EMBI_BIND_LOG_INFO, n, a, nullptr);
    }
    c->group_depth++;
}

void embi_impl_console_groupCollapsed(embi_bind_ctx *c, int n, const embi_js_value *a)
{
    embi_impl_console_group(c, n, a);
}

void embi_impl_console_groupEnd(embi_bind_ctx *c)
{
    if (c->group_depth) {
        c->group_depth--;
    }
}

void embi_impl_console_time(embi_bind_ctx *c, embi_dstr a0)
{
    embi_bind_timer_start(c, embi_bind_utf8(c, a0));
}

static void time_report(embi_bind_ctx *c, embi_dstr a0, bool stop, int n, const embi_js_value *a)
{
    embi_str label = embi_bind_utf8(c, a0);
    double ms = embi_bind_timer_end(c, label, stop);
    if (ms < 0) {
        embi_sb sb = {0};
        embi_sb_append(&sb, EMBI_STR("Timer '"));
        embi_sb_append(&sb, label);
        embi_sb_append(&sb, EMBI_STR("' does not exist"));
        embi_bind_log(c, EMBI_BIND_LOG_WARN, (embi_str){sb.data, sb.len});
        embi_sb_free(&sb);
        return;
    }
    label_line(c, EMBI_BIND_LOG_INFO, label, ": %.3fms", ms, n, a);
}

void embi_impl_console_timeLog(embi_bind_ctx *c, embi_dstr a0, int n, const embi_js_value *a)
{
    time_report(c, a0, false, n, a);
}

void embi_impl_console_timeEnd(embi_bind_ctx *c, embi_dstr a0)
{
    time_report(c, a0, true, 0, nullptr);
}
