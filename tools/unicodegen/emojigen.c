/*
 * emojigen: builds src/js/regexp_emoji.inc, the members of the properties of strings behind RegExp
 * \p{RGI_Emoji} and friends (ECMA-262 22.2.2.9, v flag).
 *
 * Usage: emojigen <ucd-dir> <output.inc>
 *   <ucd-dir> must hold the Unicode emoji data files emoji-sequences.txt and
 *   emoji-zwj-sequences.txt (https://www.unicode.org/Public/emoji/<version>/; the checked-in
 *   table is from 17.0). They are not kept in tools/unicodegen/data: drop them there to regenerate.
 *
 * Output layout (consumed by emoji_prop in src/js/regexp.c):
 *   emo_basic1     ranges (lo, hi pairs) of single code points of Basic_Emoji
 *   emo_basic2     ranges of code points that are in Basic_Emoji as "cp FE0F"
 *   emo_mod_bases  the bases of RGI_Emoji_Modifier_Sequence (each takes all five modifiers)
 *   emo_flags      pairs of regional indicator offsets (0 = A) of RGI_Emoji_Flag_Sequence
 *   emo_tags       the tag letters of RGI_Emoji_Tag_Sequence (between U+1F3F4 and U+E007F)
 *   emo_zwj_tok    the distinct ZWJ components, in order of first use: length, code points
 *   emo_zwj_seq    the RGI_Emoji_ZWJ_Sequence lines in file order: component count, token indexes
 * Emoji_Keycap_Sequence is the fixed set [0-9#*] FE0F 20E3 and is checked, not stored.
 *
 * This is a host tool and may use stdio. It runs only through the 'unicodeemoji' CMake target.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *v;
    size_t n, cap;
} u32vec;

static void push(u32vec *a, uint32_t x)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 64;
        a->v = realloc(a->v, a->cap * sizeof *a->v);
        if (!a->v) {
            fprintf(stderr, "emojigen: out of memory\n");
            exit(1);
        }
    }
    a->v[a->n++] = x;
}

static void die(const char *msg, const char *line)
{
    fprintf(stderr, "emojigen: %s: %s\n", msg, line);
    exit(1);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* One data line: its code point field and type field. Returns 0 for blank and comment lines. */
static int split_line(char *line, char **cps, char **type)
{
    char *h = strchr(line, '#');
    if (h) {
        *h = 0;
    }
    char *s1 = strchr(line, ';');
    if (!s1) {
        return 0;
    }
    *s1++ = 0;
    char *s2 = strchr(s1, ';');
    if (s2) {
        *s2 = 0;
    }
    while (*s1 == ' ' || *s1 == '\t') {
        s1++;
    }
    size_t n = strlen(s1);
    while (n && (s1[n - 1] == ' ' || s1[n - 1] == '\t' || s1[n - 1] == '\r' || s1[n - 1] == '\n')) {
        s1[--n] = 0;
    }
    *cps = line;
    *type = s1;
    return 1;
}

/* Parses "A", "A..B" or "A B C" into out (a range gives lo and hi with *range set). */
static size_t parse_cps(const char *f, uint32_t *out, size_t max, int *range)
{
    size_t n = 0;
    *range = 0;
    const char *p = f;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *e;
        unsigned long v = strtoul(p, &e, 16);
        if (e == p || n >= max) {
            die("bad code point field", f);
        }
        out[n++] = (uint32_t)v;
        p = e;
        if (p[0] == '.' && p[1] == '.') {
            *range = 1;
            p += 2;
        }
    }
    return n;
}

static void ranges(const u32vec *in, u32vec *out)
{
    for (size_t i = 0; i < in->n; i++) {
        if (out->n && out->v[out->n - 1] + 1 == in->v[i]) {
            out->v[out->n - 1] = in->v[i];
        } else {
            push(out, in->v[i]);
            push(out, in->v[i]);
        }
    }
}

/* The array body is "    a, b, c," with single spaces between the numbers. */
static void arr_fmt(FILE *o, const char *name, const char *typ, const u32vec *a, size_t per)
{
    fprintf(o, "static const %s %s[] = {\n", typ, name);
    for (size_t i = 0; i < a->n; i += per) {
        fputs("   ", o);
        for (size_t j = i; j < i + per && j < a->n; j++) {
            fprintf(o, " %u,", a->v[j]);
        }
        fputc('\n', o);
    }
    fputs("};\n", o);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: emojigen <ucd-dir> <output.inc>\n");
        return 2;
    }
    char path[1024];
    char line[4096];
    u32vec b1 = {0}, b2 = {0}, bases = {0}, flags = {0}, keycaps = {0};
    u32vec tags = {0}; /* 1 + letters, 0 separated */
    u32vec toks = {0}; /* length, code points */
    u32vec tokoff = {0};
    u32vec seqs = {0};
    size_t nseq = 0;
    for (int pass = 0; pass < 2; pass++) {
        snprintf(path, sizeof path, "%s/%s", argv[1],
                 pass == 0 ? "emoji-sequences.txt" : "emoji-zwj-sequences.txt");
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "emojigen: cannot open %s\n", path);
            return 1;
        }
        while (fgets(line, sizeof line, f)) {
            char *cpf, *type;
            if (!split_line(line, &cpf, &type)) {
                continue;
            }
            uint32_t c[32];
            int range;
            size_t n = parse_cps(cpf, c, 32, &range);
            if (strncmp(type, "Basic_Emoji", 11) == 0) {
                uint32_t lo = c[0], hi = range ? c[1] : c[0];
                if (n == 1 || range) {
                    for (uint32_t x = lo; x <= hi; x++) {
                        push(&b1, x);
                    }
                } else if (n == 2 && c[1] == 0xFE0F) {
                    push(&b2, c[0]);
                } else {
                    die("unexpected Basic_Emoji line", cpf);
                }
                if (range && n != 2) {
                    die("bad range", cpf);
                }
            } else if (strncmp(type, "Emoji_Keycap_Sequence", 21) == 0) {
                if (n != 3 || c[1] != 0xFE0F || c[2] != 0x20E3) {
                    die("unexpected keycap line", cpf);
                }
                push(&keycaps, c[0]);
            } else if (strncmp(type, "RGI_Emoji_Flag_Sequence", 23) == 0) {
                if (n != 2 || c[0] < 0x1F1E6 || c[0] > 0x1F1FF || c[1] < 0x1F1E6 || c[1] > 0x1F1FF) {
                    die("unexpected flag line", cpf);
                }
                push(&flags, c[0] - 0x1F1E6u);
                push(&flags, c[1] - 0x1F1E6u);
            } else if (strncmp(type, "RGI_Emoji_Modifier_Sequence", 27) == 0) {
                if (n != 2 || c[1] < 0x1F3FB || c[1] > 0x1F3FF) {
                    die("unexpected modifier line", cpf);
                }
                push(&bases, c[0]);
            } else if (strncmp(type, "RGI_Emoji_Tag_Sequence", 22) == 0) {
                if (n < 3 || c[0] != 0x1F3F4 || c[n - 1] != 0xE007F) {
                    die("unexpected tag line", cpf);
                }
                push(&tags, (uint32_t)(n - 2));
                for (size_t k = 1; k + 1 < n; k++) {
                    push(&tags, c[k] - 0xE0000u);
                }
            } else if (strncmp(type, "RGI_Emoji_ZWJ_Sequence", 22) == 0) {
                /* Split at U+200D into components; each distinct one gets a token index. */
                u32vec parts = {0};
                size_t start = 0;
                for (size_t k = 0; k <= n; k++) {
                    if (k == n || c[k] == 0x200D) {
                        if (k == start) {
                            die("empty ZWJ component", cpf);
                        }
                        size_t found = (size_t)-1;
                        for (size_t t = 0; t < tokoff.n; t++) {
                            const uint32_t *tk = &toks.v[tokoff.v[t]];
                            if (tk[0] == k - start && memcmp(tk + 1, c + start, (k - start) * 4) == 0) {
                                found = t;
                                break;
                            }
                        }
                        if (found == (size_t)-1) {
                            found = tokoff.n;
                            push(&tokoff, (uint32_t)toks.n);
                            push(&toks, (uint32_t)(k - start));
                            for (size_t m = start; m < k; m++) {
                                push(&toks, c[m]);
                            }
                        }
                        push(&parts, (uint32_t)found);
                        start = k + 1;
                    }
                }
                push(&seqs, (uint32_t)parts.n);
                for (size_t k = 0; k < parts.n; k++) {
                    push(&seqs, parts.v[k]);
                }
                nseq++;
                free(parts.v);
            }
        }
        fclose(f);
    }
    /* The keycap set is fixed in the engine: check it. */
    if (keycaps.n != 12) {
        die("Emoji_Keycap_Sequence must list 12 keycaps", "emoji-sequences.txt");
    }
    qsort(b1.v, b1.n, 4, cmp_u32);
    qsort(b2.v, b2.n, 4, cmp_u32);
    qsort(bases.v, bases.n, 4, cmp_u32);
    size_t nb = 0;
    for (size_t i = 0; i < bases.n; i++) {
        if (i == 0 || bases.v[i] != bases.v[i - 1]) {
            bases.v[nb++] = bases.v[i];
        }
    }
    bases.n = nb;
    /* Flags sorted as pairs. */
    for (size_t i = 0; i + 2 < flags.n + 0; i += 2) {
        for (size_t j = i + 2; j < flags.n; j += 2) {
            if (flags.v[j] < flags.v[i] || (flags.v[j] == flags.v[i] && flags.v[j + 1] < flags.v[i + 1])) {
                uint32_t a = flags.v[i], b = flags.v[i + 1];
                flags.v[i] = flags.v[j];
                flags.v[i + 1] = flags.v[j + 1];
                flags.v[j] = a;
                flags.v[j + 1] = b;
            }
        }
    }
    u32vec r1 = {0}, r2 = {0};
    ranges(&b1, &r1);
    ranges(&b2, &r2);

    FILE *o = fopen(argv[2], "wb");
    if (!o) {
        fprintf(stderr, "emojigen: cannot write %s\n", argv[2]);
        return 1;
    }
    fputs("/* Generated by tools/unicodegen/emojigen.c from the Unicode emoji data files\n"
          " * emoji-sequences.txt and emoji-zwj-sequences.txt (the properties of strings of ECMA-262\n"
          " * p-escapes, v-mode). Do not edit by hand: cmake --build <dir> --target unicodeemoji. */\n",
          o);
    arr_fmt(o, "emo_basic1", "uint32_t", &r1, 8);
    arr_fmt(o, "emo_basic2", "uint32_t", &r2, 8);
    arr_fmt(o, "emo_mod_bases", "uint32_t", &bases, 8);
    arr_fmt(o, "emo_flags", "uint8_t", &flags, 26);
    fputs("static const char *const emo_tags[] = {", o);
    {
        size_t i = 0;
        int first = 1;
        while (i < tags.n) {
            uint32_t len = tags.v[i++];
            fprintf(o, "%s\"", first ? "" : ", ");
            first = 0;
            for (uint32_t k = 0; k < len; k++) {
                fputc((int)tags.v[i++], o);
            }
            fputc('"', o);
        }
    }
    fputs("};\n", o);
    arr_fmt(o, "emo_zwj_tok", "uint32_t", &toks, 12);
    arr_fmt(o, "emo_zwj_seq", "uint16_t", &seqs, 16);
    fprintf(o, "#define EMO_NTOK %zu\n#define EMO_NZWJ %zu\n", tokoff.n, nseq);
    fclose(o);
    return 0;
}
