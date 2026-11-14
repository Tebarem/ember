#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
/*
 * idlgen: generates the binding glue from WebIDL files (W5a).
 *
 * Usage: idlgen <gen-dir> <file.webidl>...
 *
 * Reads the files (their base names are the generated file names), parses and resolves them, and
 * writes <gen-dir>/<stem>.c and <gen-dir>/<stem>.h for each file, plus <gen-dir>/types.h and
 * <gen-dir>/ifaces.c. Exits 1 after printing the diagnostics ("file.webidl:line: message") when the
 * IDL has errors. The outputs are deterministic (sorted), so regeneration produces no diff unless
 * the IDL changed.
 */
#include "idl.h"

#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "idlgen: cannot open %s\n", path);
        exit(2);
    }
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        exit(2);
    }
    for (;;) {
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (n < cap) {
            break;
        }
        cap *= 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
            exit(2);
        }
        buf = nb;
    }
    fclose(f);
    *len = n;
    return buf;
}

/* The base name of a path without its directory and its .webidl extension. */
static void stem_of(const char *path, char *out, size_t cap)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    snprintf(out, cap, "%s", base);
    size_t n = strlen(out);
    if (n > 7 && strcmp(out + n - 7, ".webidl") == 0) {
        out[n - 7] = '\0';
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static FILE *open_out(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "idlgen: cannot write %s\n", path);
        exit(2);
    }
    return f;
}

int main(int argc, char **argv)
{
    const char *props_path = nullptr;
    if (argc >= 4 && strcmp(argv[1], "--props") == 0) {
        props_path = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: idlgen [--props props.txt] <gen-dir> <file.webidl>...\n");
        return 2;
    }
    const char *outdir = argv[1];
    int nfiles = argc - 2;
    const char **paths = (const char **)argv + 2;
    char *props_text = nullptr;
    if (props_path) {
        size_t plen = 0;
        props_text = read_file(props_path, &plen);
        props_text = realloc(props_text, plen + 1);
        props_text[plen] = '\0';
        idl_set_cssprops(props_text);
    }
    /* Parse in name order, so the database (and every output) does not depend on the order given. */
    qsort(paths, (size_t)nfiles, sizeof *paths, cmp_str);

    idl_db db = {0};
    for (int i = 0; i < nfiles; i++) {
        size_t len = 0;
        char *text = read_file(paths[i], &len);
        char stem[256];
        stem_of(paths[i], stem, sizeof stem);
        idl_parse(&db, stem, text, len);
        free(text);
    }
    idl_resolve(&db);
    if (db.nerrors) {
        fprintf(stderr, "idlgen: %d error(s), nothing written\n", db.nerrors);
        idl_db_free(&db);
        return 1;
    }
    for (int i = 0; i < nfiles; i++) {
        char stem[256];
        stem_of(paths[i], stem, sizeof stem);
        char cname[300], hname[300];
        snprintf(cname, sizeof cname, "%s.c", stem);
        snprintf(hname, sizeof hname, "%s.h", stem);
        FILE *fc = open_out(outdir, cname);
        FILE *fh = open_out(outdir, hname);
        idl_gen_file(&db, stem, fc, fh);
        fclose(fc);
        fclose(fh);
    }
    if (props_text) {
        FILE *fp = open_out(outdir, "cssprops.c");
        idl_gen_cssprops(&db, props_text, fp);
        fclose(fp);
    }
    FILE *fsc = open_out(outdir, "shapes.c");
    FILE *fsh = open_out(outdir, "shapes.h");
    idl_gen_shapes(fsc, fsh);
    fclose(fsc);
    fclose(fsh);
    FILE *ft = open_out(outdir, "types.h");
    FILE *fi = open_out(outdir, "ifaces.c");
    int errors = idl_gen_shared(&db, ft, fi);
    fclose(ft);
    fclose(fi);
    if (db.nerrors || errors) {
        fprintf(stderr, "idlgen: %d error(s)\n", db.nerrors + errors);
        idl_db_free(&db);
        return 1;
    }
    idl_db_free(&db);
    return 0;
}
