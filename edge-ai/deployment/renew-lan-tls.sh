#!/usr/bin/env bash
set -euo pipefail

tls_dir=/etc/farmwiz-orchestrator/tls
cert="$tls_dir/server.crt"
if openssl x509 -checkend 2592000 -noout -in "$cert" >/dev/null 2>&1; then
  exit 0
fi

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
umask 077

openssl ecparam -name prime256v1 -genkey -noout -out "$work_dir/server.key"
openssl req -new -key "$work_dir/server.key" \
  -out "$work_dir/server.csr" \
  -subj "/CN=192.168.100.82"
cat > "$work_dir/server.ext" <<'EOF'
[server_cert]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:192.168.100.82
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
EOF
openssl x509 -req -sha256 -days 365 \
  -in "$work_dir/server.csr" \
  -CA "$tls_dir/local-ca.crt" \
  -CAkey "$tls_dir/local-ca.key" \
  -CAcreateserial \
  -extfile "$work_dir/server.ext" \
  -extensions server_cert \
  -out "$work_dir/server.crt"
openssl verify -CAfile "$tls_dir/local-ca.crt" -verify_ip 192.168.100.82 "$work_dir/server.crt"
install -o root -g pi -m 0640 "$work_dir/server.key" "$tls_dir/server.key.new"
install -o root -g pi -m 0644 "$work_dir/server.crt" "$tls_dir/server.crt.new"
mv "$tls_dir/server.key.new" "$tls_dir/server.key"
mv "$tls_dir/server.crt.new" "$tls_dir/server.crt"
systemctl restart farmwiz-orchestrator.service
