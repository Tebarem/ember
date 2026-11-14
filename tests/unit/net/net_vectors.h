#pragma once
/* Generated once with Python's gzip and zlib from the text below (see test_http.c). */
#define NET_VEC_TEXT "The quick brown fox jumps over the lazy dog. " /* repeated 20 times */
static const unsigned char net_vec_gzip[] = {31,139,8,0,0,0,0,0,2,10,11,201,72,85,40,44,205,76,206,86,72,42,202,47,207,83,72,203,175,80,200,42,205,45,40,86,200,47,75,45,82,40,1,74,231,36,86,85,42,164,228,167,235,41,132,140,42,30,85,60,170,152,218,138,1,230,74,102,176,132,3,0,0};
static const unsigned char net_vec_zlib[] = {120,156,11,201,72,85,40,44,205,76,206,86,72,42,202,47,207,83,72,203,175,80,200,42,205,45,40,86,200,47,75,45,82,40,1,74,231,36,86,85,42,164,228,167,235,41,132,140,42,30,85,60,170,152,218,138,1,71,165,67,28};
static const unsigned char net_vec_rawdeflate[] = {11,201,72,85,40,44,205,76,206,86,72,42,202,47,207,83,72,203,175,80,200,42,205,45,40,86,200,47,75,45,82,40,1,74,231,36,86,85,42,164,228,167,235,41,132,140,42,30,85,60,170,152,218,138,1};
static const unsigned char net_vec_gzip_bad_crc[] = {31,139,8,0,0,0,0,0,2,10,11,201,72,85,40,44,205,76,206,86,72,42,202,47,207,83,72,203,175,80,200,42,205,45,40,86,200,47,75,45,82,40,1,74,231,36,86,85,42,164,228,167,235,41,132,140,42,30,85,60,170,152,218,138,1,231,74,102,176,132,3,0,0};

/* A raw deflate stream (fixed Huffman) of one zero byte followed by `matches` copies of 258 bytes: about 20:1. */
static inline void make_deflate_zeros(embi_buf *out, uint32_t matches)
{
    uint8_t cur = 0;
    int nb = 0;
#define PUT_BIT(b)                                                                                     \
    do {                                                                                               \
        cur |= (uint8_t)((b) << nb);                                                                   \
        if (++nb == 8) {                                                                               \
            (void)embi_buf_append(out, &cur, 1);                                                       \
            cur = 0;                                                                                   \
            nb = 0;                                                                                    \
        }                                                                                              \
    } while (0)
#define PUT_CODE(code, n)                                                                              \
    for (int k_ = (n) - 1; k_ >= 0; k_--) {                                                            \
        PUT_BIT(((code) >> k_) & 1);                                                                   \
    }
    PUT_BIT(1);  /* BFINAL */
    PUT_BIT(1);  /* BTYPE = 01, least significant bit first */
    PUT_BIT(0);
    PUT_CODE(0x30, 8); /* literal 0 */
    for (uint32_t i = 0; i < matches; i++) {
        PUT_CODE(0xC5, 8); /* length 258 (code 285) */
        PUT_CODE(0, 5);    /* distance 1 */
    }
    PUT_CODE(0, 7); /* end of block */
    while (nb != 0) {
        PUT_BIT(0);
    }
#undef PUT_CODE
#undef PUT_BIT
}
