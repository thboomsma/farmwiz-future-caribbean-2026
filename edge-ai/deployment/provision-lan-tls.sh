#!/usr/bin/env bash
set -euo pipefail

tls_dir=/etc/farmwiz-orchestrator/tls
ca_key="$tls_dir/local-ca.key"
ca_cert="$tls_dir/local-ca.crt"
server_key="$tls_dir/server.key"
server_cert="$tls_dir/server.crt"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
umask 077

if [[ -e "$ca_key" || -e "$ca_cert" || -e "$server_key" || -e "$server_cert" ]]; then
  echo "TLS material already exists at $tls_dir; refusing to replace it." >&2
  exit 1
fi

install -d -o root -g pi -m 0750 "$tls_dir"
openssl ecparam -name prime256v1 -genkey -noout -out "$work_dir/local-ca.key"
openssl req -new -key "$work_dir/local-ca.key" \
  -out "$work_dir/local-ca.csr" \
  -subj "/CN=FarmWiz Orchestrator LAN Root CA"
cat > "$work_dir/ca.ext" <<'EOF'
[v3_ca]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always
EOF
openssl x509 -req -sha256 -days 3650 \
  -in "$work_dir/local-ca.csr" \
  -signkey "$work_dir/local-ca.key" \
  -extfile "$work_dir/ca.ext" \
  -extensions v3_ca \
  -out "$work_dir/local-ca.crt"

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
  -CA "$work_dir/local-ca.crt" \
  -CAkey "$work_dir/local-ca.key" \
  -CAcreateserial \
  -extfile "$work_dir/server.ext" \
  -extensions server_cert \
  -out "$work_dir/server.crt"
openssl x509 -in "$work_dir/local-ca.crt" -noout -subject -issuer -ext basicConstraints -ext keyUsage
openssl x509 -in "$work_dir/server.crt" -noout -subject -issuer -ext subjectAltName
openssl verify -CAfile "$work_dir/local-ca.crt" "$work_dir/server.crt"
openssl verify -CAfile "$work_dir/local-ca.crt" -verify_ip 192.168.100.82 "$work_dir/server.crt"

install -o root -g root -m 0600 "$work_dir/local-ca.key" "$ca_key"
install -o root -g root -m 0644 "$work_dir/local-ca.crt" "$ca_cert"
install -o root -g pi -m 0640 "$work_dir/server.key" "$server_key"
install -o root -g pi -m 0644 "$work_dir/server.crt" "$server_cert"
chown root:pi "$tls_dir"
chmod 0750 "$tls_dir"

echo "Created a LAN-only TLS certificate for IP 192.168.100.82."
echo "The CA private key remains on the Pi at $ca_key."
echo "The public CA certificate is at $ca_cert."
openssl x509 -in "$server_cert" -noout -subject -issuer -dates -ext subjectAltName
