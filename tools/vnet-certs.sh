#!/bin/bash
# Certificates for a vNet deployment: one CA, then one certificate per
# machine (or per role) with the role in the subject's OU field, which the
# programs check on every link when VNET_TLS=1.
#
#   vnet-certs.sh init  <dir>                 make <dir>/ca.pem and <dir>/ca.key
#   vnet-certs.sh issue <dir> <role> <name>   make <dir>/<name>.pem and <dir>/<name>.key
#
# role is conductor, switch or agent. Every machine gets
#   VNET_TLS=1 VNET_TLS_CA=<dir>/ca.pem
#   VNET_TLS_CERT=<dir>/<name>.pem VNET_TLS_KEY=<dir>/<name>.key
# Keep ca.key off every machine; it is needed only to issue certificates.
set -euo pipefail

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

[ $# -ge 2 ] || usage
cmd="$1"; dir="$2"
DAYS="${VNET_CERT_DAYS:-400}"

case "$cmd" in
init)
    [ $# -eq 2 ] || usage
    mkdir -p "$dir"
    if [ -f "$dir/ca.pem" ]; then
        echo "$dir/ca.pem exists; not touching it" >&2
        exit 1
    fi
    (umask 077; openssl genrsa -out "$dir/ca.key" 3072 2>/dev/null)
    openssl req -x509 -new -key "$dir/ca.key" -sha256 -days "$DAYS" \
        -subj "/CN=vnet-ca/O=vnet" -out "$dir/ca.pem" 2>/dev/null
    echo "wrote $dir/ca.pem and $dir/ca.key"
    ;;
issue)
    [ $# -eq 4 ] || usage
    role="$3"; name="$4"
    case "$role" in conductor|switch|agent) ;; *) echo "role must be conductor, switch or agent" >&2; exit 2;; esac
    case "$name" in *[!A-Za-z0-9_-]*|"") echo "name must be [A-Za-z0-9_-]+" >&2; exit 2;; esac
    [ -f "$dir/ca.pem" ] && [ -f "$dir/ca.key" ] || { echo "run init first" >&2; exit 1; }
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    (umask 077; openssl genrsa -out "$dir/$name.key" 2048 2>/dev/null)
    openssl req -new -key "$dir/$name.key" -subj "/CN=$name/OU=$role/O=vnet" \
        -out "$tmp/req.pem" 2>/dev/null
    cat > "$tmp/ext.cnf" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth,clientAuth
subjectAltName=DNS:$name
EOF
    openssl x509 -req -in "$tmp/req.pem" -CA "$dir/ca.pem" -CAkey "$dir/ca.key" \
        -CAcreateserial -days "$DAYS" -sha256 -extfile "$tmp/ext.cnf" \
        -out "$dir/$name.pem" 2>/dev/null
    echo "wrote $dir/$name.pem and $dir/$name.key (OU=$role)"
    ;;
*)
    usage
    ;;
esac
