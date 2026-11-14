// fuzz-modules: bindings loader js dom base
/* multipart/form-data and application/x-www-form-urlencoded bodies through the FormData parser of the bindings
 * (ldr_formdata_parse, the path of Response.formData() and Request.formData()), with a real binding context.
 * Input: byte 0 = mode (0: content type "multipart/form-data; boundary=<piece>", 1: the piece is the whole content
 * type, 2: "application/x-www-form-urlencoded"); the piece runs to the first NUL byte; the rest is the body. */
#include "fuzz_common.h"

#include "bindings/bind.h"
#include "bindings/impl_loader.h"
#include "bindings/loader_host.h"
#include "dom/dom.h"
#include "js/js.h"
#include "js/jobs.h"

static void one(const uint8_t *data, size_t size)
{
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    const uint8_t *nul = in.n ? memchr(in.p, 0, in.n) : nullptr;
    size_t pn = nul ? (size_t)(nul - in.p) : in.n;
    if (pn > 200) {
        pn = 200;
    }
    char ct[320];
    int len;
    switch (mode % 3) {
    case 0:
        len = SDL_snprintf(ct, sizeof ct, "multipart/form-data; boundary=%.*s", (int)pn, (const char *)in.p);
        break;
    case 1:
        len = SDL_snprintf(ct, sizeof ct, "%.*s", (int)pn, (const char *)in.p);
        break;
    default:
        len = SDL_snprintf(ct, sizeof ct, "application/x-www-form-urlencoded");
        pn = 0;
        break;
    }
    if (len < 0 || (size_t)len >= sizeof ct) {
        len = (int)sizeof ct - 1;
    }
    size_t skip = nul ? pn + 1 : pn;
    if (mode % 3 == 2) {
        skip = 0;
    }
    if (skip > in.n) {
        skip = in.n;
    }
    const uint8_t *body = in.p + skip;
    size_t bn = in.n - skip;
    static const uint8_t empty[1] = {0};

    embi_js_runtime *rt = embi_js_runtime_new();
    embi_document *doc = embi_document_new(true);
    embi_bind_ctx *c = embi_bind_new(rt, doc);
    embi_loader_env le = {0};
    embi_loader_attach(c, &le);
    embi_js_value fd = ldr_formdata_parse(c, bn ? body : empty, bn, (embi_str){ct, (uint32_t)len});
    if (fd == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
    }
    embi_js_run_jobs(rt);
    embi_loader_detach(c);
    embi_bind_free(c);
    embi_js_runtime_free(rt);
    embi_document_free(doc);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    /* One context is made and freed first, so what the engine and the bindings allocate once per process is part of
     * the baseline of the per-input leak check. */
    static const uint8_t warm[] = "\0XB\0--XB\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nv\r\n--XB--\r\n";
    one(warm, sizeof warm - 1);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    one(data, size);
    FUZZ_GUARD_END();
    return 0;
}
