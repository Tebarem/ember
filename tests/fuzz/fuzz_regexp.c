// fuzz-modules: js jslex num base
/* The RegExp engine on its own: flags parsing, pattern compile (every flag combination), and
 * search / match_at on a short subject, with the backtracking memory cap and an interrupt hook.
 * Input: byte 0 = flag bits (EMBI_RE_FLAG_*), byte 1 = bit 0 wide subject, bits 1..7 = start
 * index; byte 2 = pattern length; then the pattern (UTF-8, converted to UTF-16) and the subject. */
#include "fuzz_common.h"

#include "js/regexp.h"

typedef struct steps {
    unsigned n;
} steps;

static bool stop_cb(void *ctx)
{
    return ++((steps *)ctx)->n > 200;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint32_t flags = fuzz_u8(&in);
    uint8_t b1 = fuzz_u8(&in);
    size_t plen = fuzz_u8(&in);
    fuzz_in pat = fuzz_take(&in, plen);
    fuzz_in subj = {in.p, in.n > 256 ? 256 : in.n};

    /* Flags text: the bytes of the pattern double as a flags string. */
    {
        uint16_t f16[16];
        uint32_t nf = pat.n < 16 ? (uint32_t)pat.n : 16;
        for (uint32_t i = 0; i < nf; i++) {
            f16[i] = pat.p[i];
        }
        uint32_t parsed = 0;
        (void)embi_re_parse_flags(f16, nf, &parsed);
    }

    uint16_t *pat16 = embi_alloc((pat.n + 1) * sizeof(uint16_t));
    size_t pn = embi_utf8_to_utf16(fuzz_str(&pat), pat16, pat.n);
    embi_re_error err = {0, nullptr};
    embi_re_prog *prog = embi_re_compile(pat16, (uint32_t)pn, flags, &err);
    if (prog) {
        uint32_t slots = embi_re_capture_slots(prog);
        uint32_t *caps = embi_alloc((size_t)slots * sizeof(uint32_t));
        uint32_t ng = embi_re_group_count(prog);
        for (uint32_t g = 1; g <= ng && g < 8; g++) {
            uint32_t nl = 0;
            (void)embi_re_group_name(prog, g, &nl);
        }
        bool wide = b1 & 1;
        uint16_t *s16 = nullptr;
        uint8_t *s8 = nullptr;
        embi_re_input input;
        if (wide) {
            s16 = embi_alloc((subj.n + 1) * sizeof(uint16_t));
            size_t sn = embi_utf8_to_utf16(fuzz_str(&subj), s16, subj.n);
            input = (embi_re_input){s16, (uint32_t)sn, true};
        } else {
            s8 = embi_alloc(subj.n + 1);
            if (subj.n) {
                memcpy(s8, subj.p, subj.n);
            }
            input = (embi_re_input){s8, (uint32_t)subj.n, false};
        }
        uint32_t start = (uint32_t)(b1 >> 1);
        if (start > input.len) {
            start = input.len;
        }
        steps st = {0};
        embi_re_limits lim = {(size_t)1 << 20, stop_cb, &st};
        (void)embi_re_search(prog, input, start, caps, &lim);
        st.n = 0;
        (void)embi_re_match_at(prog, input, start, caps, &lim);
        st.n = 0;
        (void)embi_re_search(prog, input, 0, caps, &lim);
        if (wide) {
            embi_free(s16, (subj.n + 1) * sizeof(uint16_t));
        } else {
            embi_free(s8, subj.n + 1);
        }
        embi_free(caps, (size_t)slots * sizeof(uint32_t));
        embi_re_free(prog);
    }
    embi_free(pat16, (pat.n + 1) * sizeof(uint16_t));
    FUZZ_GUARD_END();
    return 0;
}
