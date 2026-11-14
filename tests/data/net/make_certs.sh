#!/bin/sh
# Regenerates the test certificates (see README). Usage: make_certs.sh <cert_write> <gen_key>
# cert_write is the vendored Mbed TLS program (third_party/mbedtls/programs/x509/cert_write.c) and gen_key is
# gen_key.c in this directory, both built against the vendored Mbed TLS. Validity: 2026-01-01 to 2046-01-01.
set -e
CW="$1"; GK="$2"
V="not_before=20260101000000 not_after=20460101000000"
for k in ca server other rogue-ca rogue-server; do "$GK" $k.key; done
# The test CA, and a CA nobody trusts.
"$CW" selfsign=1 issuer_key=ca.key issuer_name="CN=Ember Test CA,O=Ember Tests" is_ca=1 max_pathlen=0 serial=1 $V output_file=ca.crt
"$CW" selfsign=1 issuer_key=rogue-ca.key issuer_name="CN=Ember Rogue CA,O=Ember Tests" is_ca=1 max_pathlen=0 serial=2 $V output_file=rogue-ca.crt
# Server certificates: localhost and 127.0.0.1 (valid), other.test only (hostname mismatch), and one for localhost
# signed by the rogue CA (unknown root).
"$CW" subject_key=server.key subject_name="CN=localhost,O=Ember Tests" issuer_crt=ca.crt issuer_key=ca.key serial=3 $V \
  san="DNS:localhost;IP:127.0.0.1" key_usage=digital_signature ext_key_usage=serverAuth output_file=server.crt
"$CW" subject_key=other.key subject_name="CN=other.test,O=Ember Tests" issuer_crt=ca.crt issuer_key=ca.key serial=4 $V \
  san="DNS:other.test" key_usage=digital_signature ext_key_usage=serverAuth output_file=other.crt
"$CW" subject_key=rogue-server.key subject_name="CN=localhost,O=Ember Tests" issuer_crt=rogue-ca.crt issuer_key=rogue-ca.key serial=5 $V \
  san="DNS:localhost;IP:127.0.0.1" key_usage=digital_signature ext_key_usage=serverAuth output_file=rogue-server.crt
