#!/usr/bin/env bash
# End-to-end demo: start a mock NF cluster + the SCP, exercise every feature,
# then tear everything down. Exits non-zero if any assertion fails.
#
# Usage: scp-poc/scripts/e2e.sh [BUILD_DIR]
set -uo pipefail

POC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-/tmp/scp_objs}"
NGINX="$BUILD_DIR/nginx"
NF="$BUILD_DIR/mock-nf"
NRF="$BUILD_DIR/mock-nrf"
CURL="curl -s --http2-prior-knowledge"
PIDS=()
FAIL=0

cd "$POC_DIR"
mkdir -p logs

cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    [ -f logs/scp.pid ] && kill "$(cat logs/scp.pid 2>/dev/null)" 2>/dev/null
}
trap cleanup EXIT

assert() { # assert <desc> <haystack> <needle>
    if printf '%s' "$2" | grep -q -- "$3"; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1 (expected '$3' in: $2)"; FAIL=1
    fi
}

echo ">> starting mock NFs"
"$NF" -addr 127.0.0.1:9101 -id udm-1 -nf UDM >logs/udm1.log 2>&1 & PIDS+=($!)
"$NF" -addr 127.0.0.1:9102 -id udm-2 -nf UDM >logs/udm2.log 2>&1 & PIDS+=($!)
"$NF" -addr 127.0.0.1:9201 -id amf-1 -nf AMF >logs/amf1.log 2>&1 & PIDS+=($!)
echo ">> starting mock NRF"
"$NRF" -addr 127.0.0.1:9301 >logs/nrf.log 2>&1 & PIDS+=($!)
sleep 1

echo ">> starting SCP"
"$NGINX" -p "$POC_DIR" -c conf/scp.conf >logs/nginx.stdout 2>&1 & PIDS+=($!)
sleep 1

TOKEN="Authorization: Bearer eyJ.test.token"

echo "=============================================================="
echo " DEMO 1: direct routing (:8080) via 3gpp-Sbi-Target-apiRoot"
echo "=============================================================="

echo ">> 1. Model D routing + HTTP/2 end-to-end + load balancing"
seen=""
for i in 1 2 3 4; do
    r=$($CURL -H "3gpp-Sbi-Target-apiRoot: http://udm-set" -H "$TOKEN" \
        http://127.0.0.1:8080/nudm-sdm/v2/x)
    seen+="$r"
done
assert "HTTP/2 end-to-end" "$seen" 'HTTP/2.0'
assert "load-balanced to udm-1" "$seen" 'udm-1'
assert "load-balanced to udm-2" "$seen" 'udm-2'

echo ">> 2. Routing to a different NF set (AMF)"
r=$($CURL -H "3gpp-Sbi-Target-apiRoot: http://amf-set" -H "$TOKEN" \
    http://127.0.0.1:8080/namf-comm/v1/x)
assert "routed to amf-1" "$r" 'amf-1'

echo ">> 3. OAuth2 enforcement (missing bearer -> 401 ProblemDetails)"
r=$($CURL -i -H "3gpp-Sbi-Target-apiRoot: http://udm-set" \
    http://127.0.0.1:8080/x)
assert "401 status" "$r" 'HTTP/2 401'
assert "problem+json" "$r" 'application/problem+json'
assert "UNAUTHORIZED cause" "$r" 'UNAUTHORIZED'

echo ">> 4. Mandatory IE check (missing Target-apiRoot -> 400 ProblemDetails)"
r=$($CURL -i -H "$TOKEN" http://127.0.0.1:8080/x)
assert "400 status" "$r" 'HTTP/2 400'
assert "MANDATORY_IE_MISSING cause" "$r" 'MANDATORY_IE_MISSING'

echo "=============================================================="
echo " DEMO 2: delegated NRF discovery (:8081) via nf-type only"
echo "=============================================================="

echo ">> 5. Delegated discovery: no apiRoot, only 3gpp-Sbi-Discovery-target-nf-type"
seen=""
for i in 1 2 3 4; do
    r=$($CURL -H "3gpp-Sbi-Discovery-target-nf-type: UDM" -H "$TOKEN" \
        http://127.0.0.1:8081/nudm-sdm/v2/x)
    seen+="$r"
done
assert "discovered + HTTP/2 end-to-end" "$seen" 'HTTP/2.0'
assert "discovery LB to udm-1" "$seen" 'udm-1'
assert "discovery LB to udm-2" "$seen" 'udm-2'

echo ">> 6. Delegated discovery resolves a different NF type (AMF)"
r=$($CURL -i -H "3gpp-Sbi-Discovery-target-nf-type: AMF" -H "$TOKEN" \
    http://127.0.0.1:8081/namf-comm/v1/x)
assert "routed to amf-1" "$r" 'amf-1'
assert "discovered NF-set header" "$r" 'x-scp-discovered: amf-set'

echo ">> 7. Discovery of an unknown NF type -> NRF denies (403)"
r=$($CURL -o /dev/null -w '%{http_code}' \
    -H "3gpp-Sbi-Discovery-target-nf-type: PCF" -H "$TOKEN" \
    http://127.0.0.1:8081/x)
assert "403 from NRF discovery failure" "$r" '403'

echo ">> 8. No apiRoot and no discovery target -> 400 ProblemDetails"
r=$($CURL -i -H "$TOKEN" http://127.0.0.1:8081/x)
assert "400 status" "$r" 'HTTP/2 400'
assert "MANDATORY_IE_MISSING cause" "$r" 'MANDATORY_IE_MISSING'

echo "=============================================================="
echo " Shared: resilience"
echo "=============================================================="

echo ">> 9. NF reselection (kill udm-1, expect failover to udm-2)"
kill "${PIDS[0]}" 2>/dev/null; sleep 1
r=$($CURL -H "3gpp-Sbi-Target-apiRoot: http://udm-set" -H "$TOKEN" \
    http://127.0.0.1:8080/x)
assert "reselected to udm-2" "$r" 'udm-2'

echo
if [ "$FAIL" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
else
    echo "SOME CHECKS FAILED"
fi
exit "$FAIL"
