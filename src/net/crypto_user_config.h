/*
 * Ember's TF-PSA-Crypto configuration (N1a), appended to the library's default crypto_config.h through
 * TF_PSA_CRYPTO_USER_CONFIG_FILE. Only what TLS 1.2/1.3 with ECDHE, AES-GCM and ChaCha20-Poly1305 and the common
 * certificate chains (ECDSA P-256/384/521, RSA PKCS#1 v1.5 and PSS, SHA-2) need. Entropy is the library's own
 * OS-backed source (MBEDTLS_PSA_BUILTIN_GET_ENTROPY: BCryptGenRandom on Windows, getrandom or /dev/urandom on Linux,
 * arc4random on macOS), kept as the default.
 */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_LMS_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_PK_PARSE_EC_COMPRESSED
#undef PSA_WANT_ALG_CBC_NO_PADDING
#undef PSA_WANT_ALG_CBC_PKCS7
#undef PSA_WANT_ALG_CCM
#undef PSA_WANT_ALG_CCM_STAR_NO_TAG
#undef PSA_WANT_ALG_CMAC
#undef PSA_WANT_ALG_CFB
#undef PSA_WANT_ALG_ECB_NO_PADDING
#undef PSA_WANT_ALG_FFDH
#undef PSA_WANT_ALG_JPAKE
#undef PSA_WANT_ALG_MD5
#undef PSA_WANT_ALG_OFB
#undef PSA_WANT_ALG_PBKDF2_HMAC
#undef PSA_WANT_ALG_PBKDF2_AES_CMAC_PRF_128
#undef PSA_WANT_ALG_RIPEMD160
#undef PSA_WANT_ALG_RSA_PKCS1V15_CRYPT
#undef PSA_WANT_ALG_SHA_1
#undef PSA_WANT_ALG_SHA3_224
#undef PSA_WANT_ALG_SHA3_256
#undef PSA_WANT_ALG_SHA3_384
#undef PSA_WANT_ALG_SHA3_512
#undef PSA_WANT_ALG_SHAKE128
#undef PSA_WANT_ALG_SHAKE256
#undef PSA_WANT_ALG_STREAM_CIPHER
#undef PSA_WANT_ALG_TLS12_PSK_TO_MS
#undef PSA_WANT_ALG_TLS12_ECJPAKE_TO_PMS
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_256
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_384
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_512
#undef PSA_WANT_ECC_SECP_K1_256
#undef PSA_WANT_ECC_MONTGOMERY_448
#undef PSA_WANT_DH_RFC7919_2048
#undef PSA_WANT_DH_RFC7919_3072
#undef PSA_WANT_DH_RFC7919_4096
#undef PSA_WANT_DH_RFC7919_6144
#undef PSA_WANT_DH_RFC7919_8192
#undef PSA_WANT_KEY_TYPE_ARIA
#undef PSA_WANT_KEY_TYPE_CAMELLIA
#undef PSA_WANT_KEY_TYPE_DH_PUBLIC_KEY
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_BASIC
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_IMPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_EXPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_GENERATE
#undef PSA_WANT_KEY_TYPE_PASSWORD
#undef PSA_WANT_KEY_TYPE_PASSWORD_HASH

/* The library's mutexes are SDL's (threading_alt.h, installed by tls.c): several threads use PSA at once (the
 * network thread and, in the tests, the test servers), and the platform layer is SDL only. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

/* Time comes from SDL (platform.c): certificate validity (the wall clock, its calendar conversion) and the monotonic
 * millisecond clock. */
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
