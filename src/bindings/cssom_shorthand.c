/*
 * Computed values of the `transition` and `animation` shorthands (W6e): getComputedStyle(el).transition and .animation.
 * Each layer is written from the computed longhands in the order of the shorthand's grammar, leaving out the values that
 * equal the initial value; a layer that is left empty is written as its property or name (CSS Transitions 1 and CSS
 * Animations 1, "Serialization" of the shorthands). The delay needs the duration before it when it is not zero.
 */
#include "bindings/impl.h"
#include "css/props.h"
#include "style/style.h"

#define LAYERS_MAX 64
#define PARTS_MAX 12

typedef struct list_text {
    embi_sb sb;
    embi_str item[LAYERS_MAX];
    uint32_t n;
} list_text;

/* The computed value of a list-valued longhand as its items: the text split at the commas outside parentheses. */
static bool list_load(const embi_style *s, embi_str longhand, list_text *l)
{
    memset(l, 0, sizeof *l);
    embi_prop_id id = embi_prop_lookup(longhand);
    if (id == 0 || !embi_style_value_text(s, id, &l->sb)) {
        return false;
    }
    const char *p = l->sb.data;
    uint32_t len = (uint32_t)l->sb.len, start = 0;
    int depth = 0;
    for (uint32_t i = 0; i <= len && l->n < LAYERS_MAX; i++) {
        if (i < len && p[i] == '(') {
            depth++;
        } else if (i < len && p[i] == ')') {
            depth--;
        } else if (i == len || (p[i] == ',' && depth == 0)) {
            uint32_t b = start, e = i;
            while (b < e && p[b] == ' ') {
                b++;
            }
            while (e > b && p[e - 1] == ' ') {
                e--;
            }
            l->item[l->n++] = (embi_str){p + b, e - b};
            start = i + 1;
        }
    }
    return l->n > 0;
}

static embi_str list_at(const list_text *l, uint32_t i)
{
    return l->item[i % l->n];
}

static bool is(embi_str s, const char *lit)
{
    return embi_str_eq(s, (embi_str){lit, (uint32_t)strlen(lit)});
}

static void put_part(embi_sb *out, embi_str s)
{
    if (out->len > 0 && out->data[out->len - 1] != ' ') {
        embi_sb_append_char(out, ' ');
    }
    embi_sb_append(out, s);
}

bool embi_impl_computed_shorthand(const embi_style *s, embi_str name, embi_sb *out)
{
    bool anim = is(name, "animation");
    if (!anim && !is(name, "transition")) {
        return false;
    }
    list_text prop, dur, ease, delay, beh, iter, dir, fill, state;
    if (anim) {
        if (!list_load(s, EMBI_STR("animation-name"), &prop) || !list_load(s, EMBI_STR("animation-duration"), &dur) ||
            !list_load(s, EMBI_STR("animation-timing-function"), &ease) || !list_load(s, EMBI_STR("animation-delay"), &delay) ||
            !list_load(s, EMBI_STR("animation-iteration-count"), &iter) || !list_load(s, EMBI_STR("animation-direction"), &dir) ||
            !list_load(s, EMBI_STR("animation-fill-mode"), &fill) || !list_load(s, EMBI_STR("animation-play-state"), &state)) {
            return false;
        }
    } else if (!list_load(s, EMBI_STR("transition-property"), &prop) || !list_load(s, EMBI_STR("transition-duration"), &dur) ||
               !list_load(s, EMBI_STR("transition-timing-function"), &ease) ||
               !list_load(s, EMBI_STR("transition-delay"), &delay) || !list_load(s, EMBI_STR("transition-behavior"), &beh)) {
        return false;
    }
    for (uint32_t i = 0; i < prop.n; i++) {
        if (i > 0) {
            embi_sb_append(out, EMBI_STR(", "));
        }
        embi_str d = list_at(&dur, i), e = list_at(&ease, i), dl = list_at(&delay, i);
        size_t layer_start = out->len;
        bool delay_set = !is(dl, "0s");
        embi_str pn = list_at(&prop, i);
        if (anim) {
            if ((!is(d, "0s") && !is(d, "auto")) || delay_set) {
                put_part(out, d);
            }
            if (!is(e, "ease")) {
                put_part(out, e);
            }
            if (delay_set) {
                put_part(out, dl);
            }
            embi_str v = list_at(&iter, i);
            if (!is(v, "1")) {
                put_part(out, v);
            }
            v = list_at(&dir, i);
            if (!is(v, "normal")) {
                put_part(out, v);
            }
            v = list_at(&fill, i);
            if (!is(v, "none")) {
                put_part(out, v);
            }
            v = list_at(&state, i);
            if (!is(v, "running")) {
                put_part(out, v);
            }
            if (!is(pn, "none") || out->len == layer_start) {
                put_part(out, pn);
            }
        } else {
            if (!is(pn, "all")) {
                put_part(out, pn);
            }
            if (!is(d, "0s") || delay_set) {
                put_part(out, d);
            }
            if (!is(e, "ease")) {
                put_part(out, e);
            }
            if (delay_set) {
                put_part(out, dl);
            }
            embi_str v = list_at(&beh, i);
            if (!is(v, "normal")) {
                put_part(out, v);
            }
            if (out->len == layer_start) {
                put_part(out, pn);
            }
        }
    }
    embi_sb_free(&prop.sb);
    embi_sb_free(&dur.sb);
    embi_sb_free(&ease.sb);
    embi_sb_free(&delay.sb);
    if (anim) {
        embi_sb_free(&iter.sb);
        embi_sb_free(&dir.sb);
        embi_sb_free(&fill.sb);
        embi_sb_free(&state.sb);
    } else {
        embi_sb_free(&beh.sb);
    }
    return true;
}
