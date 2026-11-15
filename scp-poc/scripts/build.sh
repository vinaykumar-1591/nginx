#!/usr/bin/env bash
# Build nginx with the ngx_http_scp_module addon (native, HTTP/2 enabled).
#
# Usage: scp-poc/scripts/build.sh [BUILD_DIR]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-/tmp/scp_objs}"

cd "$REPO_ROOT"

echo ">> configuring (builddir=$BUILD_DIR)"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
./auto/configure \
    --builddir="$BUILD_DIR" \
    --with-http_v2_module \
    --with-debug \
    --without-http_rewrite_module \
    --without-http_gzip_module \
    --add-module=scp-poc/module

echo ">> building nginx"
make -f "$BUILD_DIR/Makefile"

echo ">> building mock NF (Go h2c)"
( cd scp-poc/harness && go build -o "$BUILD_DIR/mock-nf" nf.go )

echo ">> done:"
echo "   nginx   : $BUILD_DIR/nginx"
echo "   mock-nf : $BUILD_DIR/mock-nf"
