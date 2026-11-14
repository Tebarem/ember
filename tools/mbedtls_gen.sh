#!/usr/bin/env bash
# Regenerates src/net/mbedtls_gen/ from the third_party/mbedtls submodule with Mbed TLS's own generator scripts.
# Run by hand when bumping Mbed TLS (this is the only place Python and Perl are needed; the Ember build itself needs
# neither). Needs: perl, python3 with the packages jinja2 and jsonschema (override with PERL=... PYTHON=...).
# Running it twice gives no diff.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mb="$here/third_party/mbedtls"
tf="$mb/tf-psa-crypto"
out="$here/src/net/mbedtls_gen"
PERL="${PERL:-perl}"
PYTHON="${PYTHON:-python3}"

[ -f "$tf/framework/CMakeLists.txt" ] || { echo "run: git submodule update --init --recursive third_party/mbedtls" >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/tls" "$tmp/crypto"

# Mbed TLS (the TLS and X.509 half).
"$PERL" "$mb/scripts/generate_errors.pl" "$tf/drivers/builtin/include/mbedtls" "$mb/include/mbedtls" \
    "$mb/scripts/data_files" "$tmp/tls/error.c"
"$PERL" "$mb/scripts/generate_features.pl" "$mb/include/mbedtls" "$mb/scripts/data_files" "$tmp/tls/version_features.c"
(cd "$mb" && "$PYTHON" scripts/generate_config_checks.py "$tmp/tls")
# TF-PSA-Crypto.
(cd "$tf" && "$PYTHON" scripts/generate_driver_wrappers.py "$tmp/crypto")
(cd "$tf" && "$PYTHON" scripts/generate_config_checks.py "$tmp/crypto")

mkdir -p "$out"
rm -f "$out"/*.c "$out"/*.h
for f in error.c version_features.c mbedtls_config_check_before.h mbedtls_config_check_final.h mbedtls_config_check_user.h; do
    cp "$tmp/tls/$f" "$out/$f"
done
for f in psa_crypto_driver_wrappers.h psa_crypto_driver_wrappers_no_static.c tf_psa_crypto_config_check_before.h \
         tf_psa_crypto_config_check_final.h tf_psa_crypto_config_check_user.h; do
    cp "$tmp/crypto/$f" "$out/$f"
done
# Normalize line endings so the result does not depend on the host.
for f in "$out"/*; do sed -i 's/\r$//' "$f"; done
echo "generated into $out from $(git -C "$mb" describe --tags --always 2>/dev/null || echo mbedtls)"
