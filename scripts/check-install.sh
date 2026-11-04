#!/bin/sh
# Finite installation checks: no server, HTTP traffic or performance workload.
set -eu
install_root=${1:?Usage: check-install.sh /absolute/installation/prefix}
case "$install_root" in /*) ;; *) printf '%s\n' 'Installation prefix must be absolute.' >&2; exit 1;; esac
binary="$install_root/bin/h2o-php"
test -x "$binary"
H2O_ROOT="$install_root" "$binary" --version
for helper in start_server fetch-ocsp-response fastcgi-cgi kill-on-close setuidgid annotate-backtrace-symbols; do
    test -x "$install_root/share/h2o/$helper"
done
test -s "$install_root/share/h2o/ca-bundle.crt"
test -s "$install_root/share/doc/h2o-php/h2o-php-build-identity.txt"
"$install_root/share/h2o/start_server" --help >/dev/null
"$install_root/share/h2o/fetch-ocsp-response" --help >/dev/null
openssl crl2pkcs7 -nocrl -certfile "$install_root/share/h2o/ca-bundle.crt" -outform DER >/dev/null
if command -v ldd >/dev/null 2>&1; then
    linkage=$(ldd "$binary")
    printf '%s\n' "$linkage"
    case "$linkage" in *'not found'*) exit 1;; esac
    case "$linkage" in *libphp-zts*) ;; *) printf '%s\n' 'PHP ZTS shared dependency was not found.' >&2; exit 1;; esac
fi
printf '%s\n' 'Installed binary, H2O assets, helper execution, CA parsing and loader dependencies passed.'
