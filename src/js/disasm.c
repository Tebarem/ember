/*
 * Bytecode listing for tests and debugging: one line per op with its offset, name and operand.
 */
#include "js/bytecode.h"
#include "js/ic.h"
#include "js/string.h"

#include <stdio.h>
#include <string.h>

/* The ops whose operand indexes an inline cache site (ic.h). */
static bool is_ic_op(uint8_t op)
{
    return op == EMBI_JSOP_GET_FIELD || op == EMBI_JSOP_SET_FIELD ||
           op == EMBI_JSOP_GET_FIELD_THIS || op == EMBI_JSOP_DEFINE_FIELD ||
           op == EMBI_JSOP_SUPER_GET || op == EMBI_JSOP_GET_GLOBAL ||
           op == EMBI_JSOP_GET_GLOBAL_OPT || op == EMBI_JSOP_SET_GLOBAL;
}

/* Appends a key: its text for a flat Latin-1 or UTF-16 string, its index, or a placeholder. */
static void append_key(embi_js_key k, embi_sb *out)
{
    char buf[32];
    if (embi_js_key_is_index(k)) {
        int n = snprintf(buf, sizeof buf, "%u", embi_js_key_index(k));
        embi_sb_append(out, (embi_str){buf, (uint32_t)n});
        return;
    }
    if (!embi_js_is_string(k)) {
        embi_sb_append(out, (embi_str){"<symbol>", 8});
        return;
    }
    const embi_js_string *s = embi_js_as_string(k);
    uint32_t len = embi_js_str_len(s);
    embi_sb_append_char(out, '"');
    for (uint32_t i = 0; i < len; i++) {
        uint16_t u = embi_js_str_at(s, i);
        embi_sb_append_char(out, (u >= 0x20 && u < 0x7f) ? (char)u : '?');
    }
    embi_sb_append_char(out, '"');
}

/* The fused op a J5i superinstruction stands for (embi_js_fuse_defs), or nullptr. */
static const embi_js_fuse_def *fuse_of(uint8_t op)
{
    for (uint32_t i = 0; i < EMBI_JS_NFUSE; i++) {
        if (embi_js_fuse_defs[i].fused == op) {
            return &embi_js_fuse_defs[i];
        }
    }
    return nullptr;
}

/* Appends the operand of the op `op` at offset at (the 5-byte form, and the inline cache's key and
 * state for the ops that have one). */
static void append_operand(const embi_js_code *c, uint32_t at, uint8_t op, embi_sb *out)
{
    char line[96];
    if (embi_js_op_table[op].size != 5) {
        return;
    }
    uint32_t operand = embi_js_rd32(c->bc + at + 1);
    int n = snprintf(line, sizeof line, " %u", operand);
    embi_sb_append(out, (embi_str){line, (uint32_t)n});
    if (is_ic_op(op) && operand < c->nics) {
        /* An inline cache site: show its key and what the cache holds. The site array exists once the
         * code has run (J5n2); before that every site is empty. */
        const embi_js_ic *ic = c->ics ? &c->ics[operand] : nullptr;
        embi_sb_append(out, (embi_str){" key=", 5});
        append_key(c->ic_keys[operand], out);
        n = snprintf(line, sizeof line, " state=%u entries=%u", ic ? ic->state : 0u,
                     ic ? ic->nentries : 0u);
        embi_sb_append(out, (embi_str){line, (uint32_t)n});
    }
}

void embi_js_disasm(const embi_js_code *c, embi_sb *out)
{
    uint32_t pc = 0;
    while (pc < c->bc_len) {
        uint8_t op = c->bc[pc];
        const embi_js_op_info *info = &embi_js_op_table[op];
        char line[96];
        int n = snprintf(line, sizeof line, "%5u  %-14s", pc, info->name);
        embi_sb_append(out, (embi_str){line, (uint32_t)n});
        const embi_js_fuse_def *fd = fuse_of(op);
        if (fd) {
            /* A superinstruction: each of its two parts with its operand, in execution order. */
            uint32_t first_size = embi_js_op_table[fd->first].size;
            embi_sb_append(out, (embi_str){"  [", 3});
            embi_sb_append(out, (embi_str){embi_js_op_table[fd->first].name,
                                           (uint32_t)strlen(embi_js_op_table[fd->first].name)});
            append_operand(c, pc, fd->first, out);
            embi_sb_append(out, (embi_str){" + ", 3});
            embi_sb_append(out, (embi_str){embi_js_op_table[fd->second].name,
                                           (uint32_t)strlen(embi_js_op_table[fd->second].name)});
            append_operand(c, pc + first_size, fd->second, out);
            embi_sb_append(out, (embi_str){"]", 1});
        } else {
            append_operand(c, pc, op, out);
        }
        embi_sb_append_char(out, '\n');
        pc += info->size;
    }
}
