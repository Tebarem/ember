#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
/*
 * idlgen tests (W5a): the parser's diagnostics, overload numbering, partial merging, the
 * generator's determinism, and that the checked-in src/bindings/gen/ matches what idlgen writes.
 *
 * The tool is a C program in tools/idlgen (not a module), so its sources are included here.
 * Generator output goes through binary scratch files, so line endings are never translated.
 */
#include "emb_test.h"

#include <SDL3/SDL_iostream.h>

#include "../../../tools/idlgen/idl_parse.c"
#include "../../../tools/idlgen/idl_gen.c"

#include <string.h>

/* The real IDL files, sorted by name as idlgen reads them. */
static const char *const k_idl_files[] = {
    "canvas", "console", "crypto", "cssom", "dom", "encoding", "events", "eventsource", "eventtarget", "exception", "fetch", "fonts", "forms", "geometry", "html", "htmlcanvas", "messaging", "observers",
    "storage", "svg", "svgelement", "testing", "timers", "traversal", "uievents", "url", "websocket", "window",
};
#define N_IDL ((int)(sizeof k_idl_files / sizeof k_idl_files[0]))

/* A growable byte buffer. */
typedef struct outbuf {
    char *p;
    size_t n;
    size_t cap;
} outbuf;

static void outbuf_put(outbuf *b, const char *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) {
            cap *= 2;
        }
        char *p = SDL_malloc(cap);
        if (b->n) {
            memcpy(p, b->p, b->n);
        }
        SDL_free(b->p);
        b->p = p;
        b->cap = cap;
    }
    if (n) {
        memcpy(b->p + b->n, d, n);
    }
    b->n += n;
}

static void outbuf_free(outbuf *b)
{
    SDL_free(b->p);
    *b = (outbuf){0};
}

/* Reads a whole file. Returns nullptr when it cannot be read. */
static char *load(const char *path, size_t *len)
{
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    *len = n;
    return data;
}

static char *g_props_text;

/* The property list that [CSSProperties] expands to (read once). */
static bool load_props(void)
{
    if (!g_props_text) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s/src/css/props.txt", EMBER_SOURCE_DIR);
        size_t len = 0;
        char *raw = load(path, &len);
        if (!raw) {
            return false;
        }
        g_props_text = SDL_malloc(len + 1);
        memcpy(g_props_text, raw, len);
        g_props_text[len] = '\0';
        SDL_free(raw);
        idl_set_cssprops(g_props_text);
    }
    return true;
}

static bool load_real_idl(idl_db *db)
{
    if (!load_props()) {
        return false;
    }
    for (int i = 0; i < N_IDL; i++) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s/src/bindings/idl/%s.webidl", EMBER_SOURCE_DIR,
                     k_idl_files[i]);
        size_t len = 0;
        char *text = load(path, &len);
        if (!text) {
            return false;
        }
        idl_parse(db, k_idl_files[i], text, len);
        SDL_free(text);
    }
    return true;
}

/* A binary scratch file. */
static FILE *scratch_open(int id, char *name, size_t cap)
{
    SDL_snprintf(name, cap, "idlgen-test-%d.tmp", id);
    return fopen(name, "wb+");
}

/* Appends everything written to f to out, then closes and removes f. */
static void scratch_take(outbuf *out, FILE *f, const char *name)
{
    rewind(f);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        outbuf_put(out, chunk, n);
    }
    fclose(f);
    remove(name);
}

/* Generates one file's .c then .h into out. id keeps the scratch names apart. */
static void gen_file_into(idl_db *db, const char *stem, outbuf *out, int id)
{
    char nc[64], nh[64];
    FILE *fc = scratch_open(2 * id, nc, sizeof nc);
    FILE *fh = scratch_open(2 * id + 1, nh, sizeof nh);
    idl_gen_file(db, stem, fc, fh);
    scratch_take(out, fc, nc);
    scratch_take(out, fh, nh);
}

/* Generates the whole output: for each file its .c then its .h, then types.h and ifaces.c. The
 * order matches the checked-in files, so the buffer can be compared with them piece by piece. */
static void generate_all(idl_db *db, outbuf *out)
{
    for (int i = 0; i < N_IDL; i++) {
        gen_file_into(db, k_idl_files[i], out, i);
    }
    char np[64], nt[64], ni[64];
    FILE *fp = scratch_open(102, np, sizeof np);
    idl_gen_cssprops(db, g_props_text, fp);
    scratch_take(out, fp, np);
    /* the native functions the member tables share, collected while the files were generated */
    char nsc[64], nsh[64];
    FILE *fsc = scratch_open(103, nsc, sizeof nsc);
    FILE *fsh = scratch_open(104, nsh, sizeof nsh);
    idl_gen_shapes(fsc, fsh);
    scratch_take(out, fsc, nsc);
    scratch_take(out, fsh, nsh);
    FILE *ft = scratch_open(100, nt, sizeof nt);
    FILE *fi = scratch_open(101, ni, sizeof ni);
    idl_gen_shared(db, ft, fi);
    scratch_take(out, ft, nt);
    scratch_take(out, fi, ni);
}

EMB_TEST(idlgen_real_files_resolve_without_errors)
{
    idl_db db = {0};
    EMB_REQUIRE(load_real_idl(&db));
    EMB_CHECK_EQ_INT(db.nerrors, 0);
    idl_resolve(&db);
    EMB_CHECK_EQ_INT(db.nerrors, 0);
    idl_db_free(&db);
}

EMB_TEST(idlgen_output_is_deterministic)
{
    idl_db a = {0}, b = {0};
    EMB_REQUIRE(load_real_idl(&a));
    EMB_REQUIRE(load_real_idl(&b));
    idl_resolve(&a);
    idl_resolve(&b);
    outbuf ga = {0}, gb = {0};
    generate_all(&a, &ga);
    generate_all(&b, &gb);
    EMB_CHECK(ga.n > 0);
    EMB_CHECK(ga.n == gb.n && memcmp(ga.p, gb.p, ga.n) == 0);
    outbuf_free(&ga);
    outbuf_free(&gb);
    idl_db_free(&a);
    idl_db_free(&b);
}

/* The checked-in gen/ files must be exactly what idlgen writes for the checked-in IDL. */
EMB_TEST(idlgen_checked_in_gen_is_current)
{
    idl_db db = {0};
    EMB_REQUIRE(load_real_idl(&db));
    idl_resolve(&db);
    outbuf fresh = {0};
    generate_all(&db, &fresh);

    outbuf disk = {0};
    for (int i = 0; i < N_IDL + 5; i++) {
        /* Per file: the .c, then the .h. Then cssprops.c, shapes.c, shapes.h, types.h and ifaces.c. */
        const char *pieces[2] = {nullptr, nullptr};
        char a[64], b[64];
        if (i < N_IDL) {
            SDL_snprintf(a, sizeof a, "%s.c", k_idl_files[i]);
            SDL_snprintf(b, sizeof b, "%s.h", k_idl_files[i]);
            pieces[0] = a;
            pieces[1] = b;
        } else {
            static const char *const k_shared[] = {"cssprops.c", "shapes.c", "shapes.h", "types.h", "ifaces.c"};
            pieces[0] = k_shared[i - N_IDL];
        }
        for (int p = 0; p < 2 && pieces[p]; p++) {
            char path[512];
            SDL_snprintf(path, sizeof path, "%s/src/bindings/gen/%s", EMBER_SOURCE_DIR, pieces[p]);
            size_t n = 0;
            char *text = load(path, &n);
            EMB_REQUIRE(text);
            /* Line endings do not count: a Windows checkout may hold CRLF copies. */
            for (size_t k = 0; k < n; k++) {
                if (text[k] != '\r') {
                    outbuf_put(&disk, text + k, 1);
                }
            }
            SDL_free(text);
        }
    }
    EMB_CHECK_EQ_INT(disk.n, fresh.n);
    EMB_CHECK(disk.n == fresh.n && memcmp(disk.p, fresh.p, fresh.n) == 0);
    outbuf_free(&disk);
    outbuf_free(&fresh);
    idl_db_free(&db);
}

/* Parses one snippet as file "snip". Returns the number of errors. */
static int parse_snippet(idl_db *db, const char *text)
{
    return idl_parse(db, "snip", text, strlen(text));
}

EMB_TEST(idlgen_syntax_error_names_file_and_line)
{
    idl_db db = {0};
    int n = parse_snippet(&db, "interface A {\n    undefined f(;\n};\n");
    EMB_CHECK(n > 0);
    EMB_CHECK(strstr(db.first_error, "snip.webidl:2:") != nullptr);
    idl_db_free(&db);
}

EMB_TEST(idlgen_unterminated_comment_is_an_error)
{
    idl_db db = {0};
    int n = parse_snippet(&db, "interface A {};\n/* never closed\n");
    EMB_CHECK(n > 0);
    EMB_CHECK(strstr(db.first_error, "unterminated comment") != nullptr);
    idl_db_free(&db);
}

EMB_TEST(idlgen_unknown_type_is_reported_on_its_line)
{
    idl_db db = {0};
    parse_snippet(&db, "interface A {\n\n    attribute Nope x;\n};\n");
    idl_resolve(&db);
    EMB_CHECK(db.nerrors > 0);
    EMB_CHECK(strstr(db.first_error, "snip.webidl:3:") != nullptr);
    EMB_CHECK(strstr(db.first_error, "Nope") != nullptr);
    idl_db_free(&db);
}

EMB_TEST(idlgen_unknown_parent_is_reported)
{
    idl_db db = {0};
    parse_snippet(&db, "interface B : Missing {\n};\n");
    idl_resolve(&db);
    EMB_CHECK(db.nerrors > 0);
    EMB_CHECK(strstr(db.first_error, "Missing") != nullptr);
    idl_db_free(&db);
}

EMB_TEST(idlgen_overloads_are_numbered_in_order)
{
    idl_db db = {0};
    parse_snippet(&db, "interface A {\n    undefined f(long a);\n    undefined f(DOMString a, long b);\n"
                       "    undefined g();\n};\n");
    idl_resolve(&db);
    EMB_CHECK_EQ_INT(db.nerrors, 0);
    idl_iface *a = idl_find_iface(&db, "A");
    EMB_REQUIRE(a);
    int seen_f = 0;
    for (int i = 0; i < a->members.n; i++) {
        idl_member *m = &a->members.items[i];
        if (m->kind != IDL_M_OP) {
            continue;
        }
        if (strcmp(m->name, "f") == 0) {
            EMB_CHECK_EQ_INT(m->noverloads, 2);
            EMB_CHECK_EQ_INT(m->overload, seen_f);
            seen_f++;
        } else if (strcmp(m->name, "g") == 0) {
            EMB_CHECK_EQ_INT(m->noverloads, 1);
        }
    }
    EMB_CHECK_EQ_INT(seen_f, 2);
    idl_db_free(&db);
}

EMB_TEST(idlgen_partial_interface_members_merge_into_the_main_record)
{
    idl_db db = {0};
    parse_snippet(&db, "interface A {\n    undefined f();\n};\npartial interface A {\n    undefined g();\n};\n");
    idl_resolve(&db);
    EMB_CHECK_EQ_INT(db.nerrors, 0);
    idl_iface *a = idl_find_iface(&db, "A");
    EMB_REQUIRE(a);
    int ops = 0;
    for (int i = 0; i < a->members.n; i++) {
        ops += a->members.items[i].kind == IDL_M_OP;
    }
    EMB_CHECK_EQ_INT(ops, 2);
    idl_db_free(&db);
}

/* Generates one snippet and returns the .c and .h text concatenated (NUL terminated). */
static char *gen_snippet(const char *text, int *errors, int id)
{
    idl_db db = {0};
    parse_snippet(&db, text);
    idl_resolve(&db);
    char nc[64], nh[64];
    FILE *fc = scratch_open(id, nc, sizeof nc);
    FILE *fh = scratch_open(id + 1, nh, sizeof nh);
    *errors = db.nerrors + idl_gen_file(&db, "snip", fc, fh);
    outbuf out = {0};
    scratch_take(&out, fc, nc);
    scratch_take(&out, fh, nh);
    /* the shared natives are part of what a snippet generates */
    char ns1[64], ns2[64];
    FILE *fsc = scratch_open(id + 2, ns1, sizeof ns1);
    FILE *fsh = scratch_open(id + 3, ns2, sizeof ns2);
    idl_gen_shapes(fsc, fsh);
    scratch_take(&out, fsc, ns1);
    scratch_take(&out, fsh, ns2);
    outbuf_put(&out, "", 1);
    idl_db_free(&db);
    return out.p;
}

EMB_TEST(idlgen_sequence_record_promise_and_nullable_types_generate)
{
    int errors = -1;
    char *g = gen_snippet("[Exposed=Window, BindObj]\ninterface A {\n"
                          "    sequence<long> s(sequence<DOMString> v);\n"
                          "    record<DOMString, long> r(record<DOMString, long> v);\n"
                          "    Promise<long> p(long v);\n"
                          "    DOMString? n(DOMString? v);\n};\n",
                          &errors, 500);
    EMB_CHECK_EQ_INT(errors, 0);
    EMB_CHECK(strstr(g, "embi_impl_A_s(") != nullptr);
    EMB_CHECK(strstr(g, "embi_bind_seq_open") != nullptr);
    EMB_CHECK(strstr(g, "embi_bind_rec_open") != nullptr);
    EMB_CHECK(strstr(g, "embi_bind_conv_nstr") != nullptr);
    SDL_free(g);
}

EMB_TEST(idlgen_overloads_generate_a_dispatcher_and_numbered_impls)
{
    int errors = -1;
    char *g = gen_snippet("[Exposed=Window, BindObj]\ninterface A {\n"
                          "    DOMString f(DOMString a);\n    DOMString f(long a);\n"
                          "    DOMString f(boolean a, DOMString b);\n};\n",
                          &errors, 510);
    EMB_CHECK_EQ_INT(errors, 0);
    EMB_CHECK(strstr(g, "embi_impl_A_f_0(") != nullptr);
    EMB_CHECK(strstr(g, "embi_impl_A_f_1(") != nullptr);
    EMB_CHECK(strstr(g, "embi_impl_A_f_2(") != nullptr);
    EMB_CHECK(strstr(g, "embi_bind_dispatch") != nullptr);
    SDL_free(g);
}

EMB_TEST(idlgen_unions_and_iterables_generate)
{
    int errors = -1;
    char *g = gen_snippet("[Exposed=Window, BindObj]\ninterface A {\n"
                          "    (DOMString or long) u((DOMString or long or boolean) v);\n"
                          "    iterable<long, DOMString>;\n};\n",
                          &errors, 520);
    EMB_CHECK_EQ_INT(errors, 0);
    EMB_CHECK(strstr(g, "embi_bind_conv_union") != nullptr);
    EMB_CHECK(strstr(g, "embi_impl_A_iter_size") != nullptr);
    SDL_free(g);
}

EMB_TEST(idlgen_integer_conversion_attributes_pick_the_conversion_kind)
{
    int errors = -1;
    char *g = gen_snippet("[Exposed=Window, BindObj]\ninterface A {\n"
                          "    [Clamp] attribute octet c;\n"
                          "    [EnforceRange] attribute unsigned long long e;\n"
                          "    attribute unsigned long long w;\n};\n",
                          &errors, 530);
    EMB_CHECK_EQ_INT(errors, 0);
    EMB_CHECK(strstr(g, "embi_impl_A_set_c(") != nullptr);
    EMB_CHECK(strstr(g, "uint8_t") != nullptr);
    EMB_CHECK(strstr(g, "uint64_t") != nullptr);
    SDL_free(g);
}

EMB_TEST(idlgen_required_dictionary_member_and_enum_generate)
{
    int errors = -1;
    char *g = gen_snippet("dictionary D { required DOMString must; long n = 4; };\n"
                          "enum E { \"a\", \"b\" };\n"
                          "[Exposed=Window, BindObj]\ninterface A {\n"
                          "    undefined f(D d);\n    attribute E? mode;\n};\n",
                          &errors, 540);
    EMB_CHECK_EQ_INT(errors, 0);
    EMB_CHECK(strstr(g, "embi_bind_dict_required") != nullptr);
    EMB_CHECK(strstr(g, "embi_bind_conv_enum") != nullptr);
    SDL_free(g);
}

EMB_TEST(idlgen_syntax_errors_in_members_name_the_line)
{
    idl_db db = {0};
    int n = parse_snippet(&db, "interface A {\n    attribute long ;\n};\n");
    EMB_CHECK(n > 0);
    EMB_CHECK(strstr(db.first_error, "snip.webidl:2:") != nullptr);
    idl_db_free(&db);
}

EMB_TEST(idlgen_generation_is_stable_for_a_snippet)
{
    const char *text = "[Exposed=Window]\ninterface A {\n    readonly attribute DOMString x;\n};\n";
    idl_db a = {0}, b = {0};
    parse_snippet(&a, text);
    parse_snippet(&b, text);
    idl_resolve(&a);
    idl_resolve(&b);
    outbuf ga = {0}, gb = {0};
    gen_file_into(&a, "snip", &ga, 400);
    gen_file_into(&b, "snip", &gb, 400);
    {
        /* both runs registered the same shared natives (once); empty the collection */
        outbuf shapes = {0};
        char n1[64], n2[64];
        FILE *f1 = scratch_open(410, n1, sizeof n1);
        FILE *f2 = scratch_open(411, n2, sizeof n2);
        idl_gen_shapes(f1, f2);
        scratch_take(&shapes, f1, n1);
        scratch_take(&shapes, f2, n2);
        EMB_CHECK(shapes.n > 0);
        outbuf_free(&shapes);
    }
    EMB_CHECK(ga.n > 0);
    EMB_CHECK(ga.n == gb.n && memcmp(ga.p, gb.p, ga.n) == 0);
    outbuf_free(&ga);
    outbuf_free(&gb);
    idl_db_free(&a);
    idl_db_free(&b);
}

EMB_TEST(idlgen_members_of_a_global_ancestor_accept_a_nullish_this)
{
    int errors = -1;
    char *g = gen_snippet("[Exposed=Window, BindObj]\ninterface Base {\n    undefined ping();\n};\n"
                          "[Exposed=Window, BindObj, Global]\ninterface Win : Base {\n    undefined own();\n};\n",
                          &errors, 520);
    EMB_CHECK_EQ_INT(errors, 0);
    /* the shared native of Base.ping (its comment names the first member with that body) says that a
     * missing this means the global object */
    const char *ping = strstr(g, "/* Base.ping */");
    EMB_REQUIRE(ping != nullptr);
    const char *next = strstr(ping + 1, "/* ");
    const char *nullish = strstr(ping, "EMBI_SELF_GLOBAL");
    EMB_CHECK(nullish != nullptr && (next == nullptr || nullish < next));
    SDL_free(g);
    g = gen_snippet("[Exposed=Window, BindObj]\ninterface Base {\n    undefined ping();\n};\n"
                    "[Exposed=Window, BindObj]\ninterface Win : Base {\n    undefined own();\n};\n",
                    &errors, 530);
    EMB_CHECK_EQ_INT(errors, 0);
    EMB_CHECK(strstr(g, "EMBI_SELF_GLOBAL") == nullptr);
    SDL_free(g);
}

EMB_TEST_MAIN()
