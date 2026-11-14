/*
 * Writes a new ECDSA P-256 private key as PEM (PKCS#8/SEC1 as Mbed TLS writes it) to the file named on the command
 * line. Used once to make the test keys (see README); built against the vendored Mbed TLS.
 *   gen_key out.key
 */
#include <mbedtls/pk.h>
#include <psa/crypto.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: gen_key out.key\n");
        return 2;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return 1;
    }
    psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&at, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&at, 256);
    psa_set_key_usage_flags(&at, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&at, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id;
    if (psa_generate_key(&at, &id) != PSA_SUCCESS) {
        return 1;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_copy_from_psa(id, &pk) != 0) {
        return 1;
    }
    unsigned char buf[1024];
    if (mbedtls_pk_write_key_pem(&pk, buf, sizeof buf) != 0) {
        return 1;
    }
    FILE *f = fopen(argv[1], "wb");
    if (f == NULL) {
        return 1;
    }
    fputs((char *)buf, f);
    fclose(f);
    return 0;
}
