/*
 * Ember's Mbed TLS configuration (N1a): appended to the library's default mbedtls_config.h through
 * MBEDTLS_USER_CONFIG_FILE. TLS 1.2 and 1.3 client with certificate verification (always required), plus the
 * server half, which only the tests use. Everything that needs the OS beyond SDL (files, sockets, timing, key
 * storage) and everything legacy (DTLS, PSK, RSA key exchange, CBC and other old ciphers) is off.
 */
#ifndef EMBER_NET_TEST_SERVER
/* The server half is only for the tests (EMBER_TESTS defines EMBER_NET_TEST_SERVER). */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_TICKET_C
#endif
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
#undef MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#undef MBEDTLS_SSL_KEYING_MATERIAL_EXPORT
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
