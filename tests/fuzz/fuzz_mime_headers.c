// fuzz-modules: loader base js
/* MIME type parsing, HTTP header names and values, Headers lists under every guard, request
 * methods, response status text, path to MIME type. Input: byte 0 = mode; the rest is split at NUL
 * bytes into pieces that act as name/value/etc. Checked invariant: a MIME type that parses
 * serializes to a string that parses to the same essence (serialization is stable). */
#include "fuzz_common.h"

#include "loader/fetch.h"
#include "loader/loader.h"

static embi_str piece(fuzz_in *in)
{
    const uint8_t *nul = in->n ? memchr(in->p, 0, in->n) : nullptr;
    size_t n = nul ? (size_t)(nul - in->p) : in->n;
    static const char empty[1] = {0};
    embi_str s = {n ? (const char *)in->p : empty, (uint32_t)n};
    in->p += n;
    in->n -= n;
    if (nul) {
        in->p++;
        in->n--;
    }
    return s;
}

static embi_str sb_str(const embi_sb *sb)
{
    return (embi_str){sb->data ? sb->data : "", sb->len};
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    if ((mode & 1) == 0) {
        embi_str text = piece(&in);
        embi_mime m = {0};
        if (embi_mime_parse(text, &m)) {
            embi_sb ser = {0};
            embi_mime_serialize(&m, &ser);
            embi_sb ess = {0};
            embi_mime_essence(&m, &ess);
            (void)embi_mime_essence_is_javascript(sb_str(&ess));
            (void)embi_mime_essence_is_json(sb_str(&ess));
            (void)embi_mime_param_value(&m, EMBI_STR("charset"));
            (void)embi_mime_param_value(&m, piece(&in));
            embi_mime again = {0};
            if (!embi_mime_parse(sb_str(&ser), &again)) {
                fprintf(stderr, "serialized MIME type does not parse: %.*s\n", (int)ser.len, ser.data);
                abort();
            }
            embi_sb ess2 = {0};
            embi_mime_essence(&again, &ess2);
            if (!embi_str_eq(sb_str(&ess), sb_str(&ess2))) {
                fprintf(stderr, "MIME essence changed through serialize: %.*s\n", (int)ser.len, ser.data);
                abort();
            }
            embi_sb_free(&ess2);
            embi_mime_free(&again);
            embi_sb_free(&ess);
            embi_sb_free(&ser);
        }
        embi_mime_free(&m);
        (void)embi_mime_from_path(text);
    } else {
        embi_headers h = {0};
        h.guard = (embi_guard)((mode >> 1) % 5);
        unsigned ops = 0;
        while (in.n > 0 && ops++ < 24) {
            uint8_t op = fuzz_u8(&in) % 8;
            embi_str name = piece(&in);
            embi_str value = piece(&in);
            embi_sb out = {0};
            switch (op) {
            case 0:
                (void)embi_headers_append(&h, name, value);
                break;
            case 1:
                (void)embi_headers_set(&h, name, value);
                break;
            case 2:
                (void)embi_headers_delete(&h, name);
                break;
            case 3:
                (void)embi_headers_has(&h, name);
                (void)embi_headers_get(&h, name, &out);
                (void)embi_headers_first(&h, name);
                break;
            case 4: {
                embi_headers sorted = {0};
                if (embi_headers_sorted(&h, &sorted)) {
                    embi_headers_free(&sorted);
                } else {
                    embi_headers_free(&sorted);
                }
                embi_headers copy = {0};
                (void)embi_headers_copy_filtered(&h, &copy, (op & 1) != 0);
                embi_headers_free(&copy);
                break;
            }
            case 5:
                (void)embi_header_name_ok(name);
                (void)embi_header_value_ok(value);
                (void)embi_header_forbidden_request(name);
                (void)embi_header_forbidden_response(name);
                (void)embi_header_trim(value);
                break;
            case 6:
                embi_header_value_to_wire(value, &out);
                embi_sb_clear(&out);
                embi_header_value_from_wire(value, &out);
                break;
            default:
                (void)embi_method_normalize(name, &out);
                (void)embi_status_text_ok(value);
                embi_headers_add_raw(&h, name, value);
                break;
            }
            embi_sb_free(&out);
        }
        embi_headers_free(&h);
    }
    FUZZ_GUARD_END();
    return 0;
}
