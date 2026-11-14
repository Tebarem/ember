/*
 * The text editing model of the form controls (W5e): see edit.h. Offsets are bytes of UTF-8 and always fall on code point
 * boundaries; the UTF-16 view is computed on demand (values are short, so the linear scans are fine).
 */
#include "platform/edit.h"

#include <string.h>

/* ------------------------------------------------------------------ UTF-8 helpers */

static const uint8_t *bytes(const embi_edit *e)
{
    return (const uint8_t *)e->text.data;
}

static uint32_t lead_len(uint8_t b)
{
    if (b < 0x80u) {
        return 1;
    }
    if (b < 0xE0u) {
        return 2;
    }
    if (b < 0xF0u) {
        return 3;
    }
    return 4;
}

static uint32_t prev_boundary(const embi_edit *e, uint32_t i)
{
    const uint8_t *d = bytes(e);
    if (i == 0) {
        return 0;
    }
    i--;
    while (i > 0 && (d[i] & 0xC0u) == 0x80u) {
        i--;
    }
    return i;
}

static uint32_t next_boundary(const embi_edit *e, uint32_t i)
{
    const uint8_t *d = bytes(e);
    uint32_t n = e->text.len;
    if (i >= n) {
        return n;
    }
    i++;
    while (i < n && (d[i] & 0xC0u) == 0x80u) {
        i++;
    }
    return i;
}

/* The code point that starts at byte i (the text is valid UTF-8 by construction). */
static uint32_t cp_at(const embi_edit *e, uint32_t i)
{
    const uint8_t *d = bytes(e);
    uint32_t n = lead_len(d[i]);
    if (n == 1) {
        return d[i];
    }
    uint32_t cp = d[i] & (0xFFu >> (n + 1));
    for (uint32_t k = 1; k < n && i + k < e->text.len; k++) {
        cp = (cp << 6) | (d[i + k] & 0x3Fu);
    }
    return cp;
}

/* Word characters: ASCII letters and digits, the underscore, and every non-ASCII code point (letters of other scripts). */
static bool word_cp(uint32_t cp)
{
    return cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp >= 0x80u;
}

static uint32_t minu(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t maxu(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

/* ------------------------------------------------------------------ storage */

void embi_edit_free(embi_edit *e)
{
    embi_sb_free(&e->text);
    e->anchor = 0;
    e->focus = 0;
    e->dir = EMBI_EDIT_DIR_NONE;
}

embi_str embi_edit_value(const embi_edit *e)
{
    return (embi_str){e->text.len > 0 ? e->text.data : nullptr, e->text.len};
}

/* Builds a new buffer from a list of pieces and makes it the value. */
static bool rebuild(embi_edit *e, uint32_t from, uint32_t to, const char *s, uint32_t n, uint32_t *caret)
{
    embi_sb tmp = {0};
    if (from > 0) {
        embi_sb_append(&tmp, (embi_str){e->text.data, from});
    }
    if (n > 0) {
        embi_sb_append(&tmp, (embi_str){s, n});
    }
    if (to < e->text.len) {
        embi_sb_append(&tmp, (embi_str){e->text.data + to, e->text.len - to});
    }
    uint32_t want = from + n + (to < e->text.len ? e->text.len - to : 0);
    if (tmp.len != want) { /* an append failed: keep the old text */
        embi_sb_free(&tmp);
        return false;
    }
    embi_sb_free(&e->text);
    e->text = tmp;
    if (caret != nullptr) {
        *caret = from + n;
    }
    return true;
}

/* Replaces [from, to) with s and puts the caret after it. */
static bool splice(embi_edit *e, uint32_t from, uint32_t to, const char *s, uint32_t n)
{
    uint32_t caret = 0;
    if (!rebuild(e, from, to, s, n, &caret)) {
        return false;
    }
    e->anchor = caret;
    e->focus = caret;
    e->dir = EMBI_EDIT_DIR_NONE;
    return true;
}

bool embi_edit_set_value(embi_edit *e, embi_str v)
{
    embi_sb tmp = {0};
    if (v.len > 0) {
        embi_sb_append(&tmp, v);
    }
    if (tmp.len != v.len) { /* an append failed: keep the old text */
        embi_sb_free(&tmp);
        return false;
    }
    embi_sb_free(&e->text);
    e->text = tmp;
    e->anchor = e->text.len;
    e->focus = e->text.len;
    e->dir = EMBI_EDIT_DIR_NONE;
    return true;
}

/* ------------------------------------------------------------------ offsets */

uint32_t embi_edit_len16(const embi_edit *e)
{
    return embi_edit_to16(e, e->text.len);
}

uint32_t embi_edit_to16(const embi_edit *e, uint32_t byte)
{
    const uint8_t *d = bytes(e);
    uint32_t n = 0;
    uint32_t lim = minu(byte, e->text.len);
    for (uint32_t i = 0; i < lim; i++) {
        if ((d[i] & 0xC0u) == 0x80u) {
            continue; /* continuation byte */
        }
        n += d[i] >= 0xF0u ? 2u : 1u; /* a four-byte sequence is a surrogate pair */
    }
    return n;
}

uint32_t embi_edit_to_byte(const embi_edit *e, uint32_t u16)
{
    const uint8_t *d = bytes(e);
    uint32_t i = 0;
    uint32_t u = 0;
    while (i < e->text.len) {
        uint32_t w = d[i] >= 0xF0u ? 2u : 1u;
        if (u + w > u16) {
            break; /* inside a surrogate pair: the boundary before it */
        }
        u += w;
        i += lead_len(d[i]);
    }
    return minu(i, e->text.len);
}

void embi_edit_get_selection(const embi_edit *e, uint32_t *start16, uint32_t *end16, int *dir)
{
    uint32_t a = minu(e->anchor, e->focus);
    uint32_t b = maxu(e->anchor, e->focus);
    if (start16 != nullptr) {
        *start16 = embi_edit_to16(e, a);
    }
    if (end16 != nullptr) {
        *end16 = embi_edit_to16(e, b);
    }
    if (dir != nullptr) {
        *dir = e->dir;
    }
}

void embi_edit_set_selection(embi_edit *e, uint32_t start16, uint32_t end16, int dir)
{
    uint32_t len16 = embi_edit_len16(e);
    start16 = minu(start16, len16);
    end16 = minu(end16, len16);
    if (start16 > end16) {
        uint32_t t = start16;
        start16 = end16;
        end16 = t;
    }
    uint32_t a = embi_edit_to_byte(e, start16);
    uint32_t b = embi_edit_to_byte(e, end16);
    if (dir == EMBI_EDIT_DIR_BACKWARD) {
        e->anchor = b;
        e->focus = a;
    } else {
        e->anchor = a;
        e->focus = b;
    }
    e->dir = dir == EMBI_EDIT_DIR_BACKWARD ? EMBI_EDIT_DIR_BACKWARD
                                           : (dir == EMBI_EDIT_DIR_FORWARD ? EMBI_EDIT_DIR_FORWARD : EMBI_EDIT_DIR_NONE);
}

void embi_edit_select_all(embi_edit *e)
{
    e->anchor = 0;
    e->focus = e->text.len;
    e->dir = EMBI_EDIT_DIR_NONE;
}

bool embi_edit_has_selection(const embi_edit *e)
{
    return e->anchor != e->focus;
}

embi_str embi_edit_selected(const embi_edit *e)
{
    uint32_t a = minu(e->anchor, e->focus);
    uint32_t b = maxu(e->anchor, e->focus);
    if (a == b || e->text.data == nullptr) {
        return (embi_str){nullptr, 0};
    }
    return (embi_str){e->text.data + a, b - a};
}

/* ------------------------------------------------------------------ editing */

bool embi_edit_insert(embi_edit *e, embi_str s, uint32_t max16)
{
    uint32_t a = minu(e->anchor, e->focus);
    uint32_t b = maxu(e->anchor, e->focus);
    uint32_t n = s.len;
    if (max16 > 0) {
        uint32_t removed = embi_edit_to16(e, b) - embi_edit_to16(e, a);
        uint32_t base = embi_edit_len16(e) - removed;
        uint32_t avail = max16 > base ? max16 - base : 0;
        uint32_t units = 0;
        uint32_t i = 0;
        while (i < s.len) {
            uint8_t lead = (uint8_t)s.ptr[i];
            uint32_t w = lead >= 0xF0u ? 2u : 1u;
            uint32_t cl = lead_len(lead);
            if (units + w > avail) {
                break;
            }
            units += w;
            i += cl;
        }
        n = minu(i, s.len);
    }
    if (a == b && n == 0) {
        return false;
    }
    return splice(e, a, b, s.ptr, n);
}

bool embi_edit_delete(embi_edit *e, bool forward)
{
    if (embi_edit_has_selection(e)) {
        return splice(e, minu(e->anchor, e->focus), maxu(e->anchor, e->focus), nullptr, 0);
    }
    uint32_t c = e->focus;
    if (forward) {
        if (c >= e->text.len) {
            return false;
        }
        return splice(e, c, next_boundary(e, c), nullptr, 0);
    }
    if (c == 0) {
        return false;
    }
    return splice(e, prev_boundary(e, c), c, nullptr, 0);
}

/* The start of the word before i (Windows rules: skip the separators, then the word). */
static uint32_t word_left(const embi_edit *e, uint32_t i)
{
    uint32_t j = i;
    while (j > 0) {
        uint32_t p = prev_boundary(e, j);
        if (word_cp(cp_at(e, p))) {
            break;
        }
        j = p;
    }
    while (j > 0) {
        uint32_t p = prev_boundary(e, j);
        if (!word_cp(cp_at(e, p))) {
            break;
        }
        j = p;
    }
    return j;
}

/* The end of the word after i. */
static uint32_t word_right(const embi_edit *e, uint32_t i)
{
    uint32_t j = i;
    while (j < e->text.len && !word_cp(cp_at(e, j))) {
        j = next_boundary(e, j);
    }
    while (j < e->text.len && word_cp(cp_at(e, j))) {
        j = next_boundary(e, j);
    }
    return j;
}

bool embi_edit_delete_word(embi_edit *e, bool forward)
{
    if (embi_edit_has_selection(e)) {
        return splice(e, minu(e->anchor, e->focus), maxu(e->anchor, e->focus), nullptr, 0);
    }
    uint32_t c = e->focus;
    if (forward) {
        uint32_t to = word_right(e, c);
        return to != c && splice(e, c, to, nullptr, 0);
    }
    uint32_t from = word_left(e, c);
    return from != c && splice(e, from, c, nullptr, 0);
}

void embi_edit_move(embi_edit *e, embi_edit_motion m, bool extend)
{
    uint32_t a = minu(e->anchor, e->focus);
    uint32_t b = maxu(e->anchor, e->focus);
    uint32_t c = e->focus;
    uint32_t to = c;
    switch (m) {
    case EMBI_EDIT_CHAR_LEFT:
        if (!extend && a != b) {
            to = a;
        } else if (c > 0) {
            to = prev_boundary(e, c);
        }
        break;
    case EMBI_EDIT_CHAR_RIGHT:
        if (!extend && a != b) {
            to = b;
        } else if (c < e->text.len) {
            to = next_boundary(e, c);
        }
        break;
    case EMBI_EDIT_WORD_LEFT:
        to = word_left(e, c);
        break;
    case EMBI_EDIT_WORD_RIGHT:
        to = word_right(e, c);
        break;
    case EMBI_EDIT_LINE_START: {
        const uint8_t *d = bytes(e);
        to = c;
        while (to > 0 && d[to - 1] != '\n') {
            to--;
        }
        break;
    }
    case EMBI_EDIT_LINE_END: {
        const uint8_t *d = bytes(e);
        to = c;
        while (to < e->text.len && d[to] != '\n') {
            to++;
        }
        break;
    }
    case EMBI_EDIT_TEXT_START:
        to = 0;
        break;
    case EMBI_EDIT_TEXT_END:
        to = e->text.len;
        break;
    case EMBI_EDIT_LINE_UP:
    case EMBI_EDIT_LINE_DOWN: {
        const uint8_t *d = bytes(e);
        uint32_t ls = c; /* the start of the line of the caret, and the column in code points */
        while (ls > 0 && d[ls - 1] != '\n') {
            ls--;
        }
        uint32_t col = 0;
        for (uint32_t i = ls; i < c; i++) {
            col += (d[i] & 0xC0u) != 0x80u ? 1u : 0u;
        }
        uint32_t t0, t1; /* the target line [t0, t1) */
        if (m == EMBI_EDIT_LINE_UP) {
            if (ls == 0) {
                to = 0;
                break;
            }
            t1 = ls - 1;
            t0 = t1;
            while (t0 > 0 && d[t0 - 1] != '\n') {
                t0--;
            }
        } else {
            t0 = c;
            while (t0 < e->text.len && d[t0] != '\n') {
                t0++;
            }
            if (t0 >= e->text.len) {
                to = e->text.len;
                break;
            }
            t0++;
            t1 = t0;
            while (t1 < e->text.len && d[t1] != '\n') {
                t1++;
            }
        }
        to = t0;
        while (col > 0 && to < t1) {
            to++;
            while (to < t1 && (d[to] & 0xC0u) == 0x80u) {
                to++;
            }
            col--;
        }
        break;
    }
    }
    if (extend) {
        e->focus = to;
    } else {
        e->anchor = to;
        e->focus = to;
    }
    if (e->focus > e->anchor) {
        e->dir = EMBI_EDIT_DIR_FORWARD;
    } else if (e->focus < e->anchor) {
        e->dir = EMBI_EDIT_DIR_BACKWARD;
    }
}

bool embi_edit_strip_newlines(embi_edit *e)
{
    const uint8_t *d = bytes(e);
    bool any = false;
    for (uint32_t i = 0; i < e->text.len; i++) {
        if (d[i] == '\n' || d[i] == '\r') {
            any = true;
            break;
        }
    }
    if (!any) {
        return false;
    }
    embi_sb tmp = {0};
    uint32_t start = 0;
    for (uint32_t i = 0; i <= e->text.len; i++) {
        bool brk = i == e->text.len || d[i] == '\n' || d[i] == '\r';
        if (!brk) {
            continue;
        }
        if (i > start) {
            embi_sb_append(&tmp, (embi_str){e->text.data + start, i - start});
        }
        start = i + 1;
    }
    /* Each removed byte is an ASCII newline: a position moves back by the number of newlines before it. */
    uint32_t anchor = e->anchor;
    uint32_t focus = e->focus;
    uint32_t before_anchor = 0;
    uint32_t before_focus = 0;
    for (uint32_t i = 0; i < e->text.len; i++) {
        if (d[i] == '\n' || d[i] == '\r') {
            if (i < anchor) {
                before_anchor++;
            }
            if (i < focus) {
                before_focus++;
            }
        }
    }
    embi_sb_free(&e->text);
    e->text = tmp;
    e->anchor = anchor - before_anchor;
    e->focus = focus - before_focus;
    return true;
}
