# nginx-scp — a 5G Service Communication Proxy on nginx (PoC)

A hackathon proof-of-concept that turns this nginx tree into a 3GPP
Release-16-aligned **Service Communication Proxy (SCP)** for the 5G Core
Service Based Interface (SBI).

It is built around the tree's **native HTTP/2 upstream** (`proxy_http_version 2`),
with a small custom module — [`ngx_http_scp_module`](module/ngx_http_scp_module.c)
— adding the SBI-specific Layer-7 logic.

> The pitch / idea write-up (problem, value, architecture, standards) is in
> [PITCH.md](PITCH.md).

## What it demonstrates

| Capability | How |
|------------|-----|
| **Model D routing** on `3gpp-Sbi-Target-apiRoot` | module parses the header → `$scp_backend` → `proxy_pass http://$scp_backend` |
| **HTTP/2 end-to-end (h2c)** | `http2 on` downstream + `proxy_http_version 2` upstream |
| **Load balancing & NF reselection** | NF-set `upstream{}` blocks + `proxy_next_upstream` |
| **OAuth2 enforcement** | `scp_require_oauth on` → `401` ProblemDetails if no bearer token (TS 33.501) |
| **Mandatory-IE validation** | missing/malformed apiRoot → `400` ProblemDetails (TS 29.571) |
| **Observability** | routing decisions logged; `X-Scp-*` response headers |

## Layout

```
scp-poc/
├── PITCH.md                     # idea write-up (problem / value / architecture)
├── module/
│   ├── ngx_http_scp_module.c    # the SCP module
│   └── config                   # nginx addon build config
├── conf/scp.conf                # demo SCP configuration
├── harness/nf.go                # mock NF producer (Go, h2c)
└── scripts/
    ├── build.sh                 # build nginx + mock NF
    └── e2e.sh                   # start cluster, assert all features, tear down
```

## The module

`ngx_http_scp_module` registers a `PREACCESS`-phase handler and four variables.

Directives (location scope):

| Directive | Default | Meaning |
|-----------|---------|---------|
| `scp on\|off;` | `off` | enable SBI processing for the location |
| `scp_require_target on\|off;` | `on` | `3gpp-Sbi-Target-apiRoot` is mandatory |
| `scp_require_oauth on\|off;` | `off` | require an `Authorization: Bearer` token |

Variables:

| Variable | Value |
|----------|-------|
| `$scp_backend` | host from the target apiRoot (matches an `upstream{}` name) |
| `$scp_target_authority` | `host[:port]` from the target apiRoot |
| `$scp_target_scheme` | scheme (`http`/`https`) |
| `$scp_has_token` | `1` if a bearer token is present, else `0` |

**Routing mechanism:** `$scp_backend` carries the *host only* (no port) so that
nginx's variabilised `proxy_pass` matches a named `upstream{}` block
(see `ngx_http_upstream_init_request` host-name match), giving load balancing
and reselection for free.

## Build

Requires: `gcc`, `make`, and `go` (for the mock NF).

```bash
scp-poc/scripts/build.sh            # outputs to /tmp/scp_objs by default
# or: scp-poc/scripts/build.sh /path/to/builddir
```

This configures nginx with `--with-http_v2_module --add-module=scp-poc/module`
and builds both `nginx` and `mock-nf`.

## Run the demo

```bash
scp-poc/scripts/e2e.sh              # builds nothing; expects build.sh already run
```

Expected output ends with `ALL CHECKS PASSED`.

### Manual run

```bash
B=/tmp/scp_objs
# 3 NF instances (2x UDM for LB, 1x AMF)
$B/mock-nf -addr 127.0.0.1:9101 -id udm-1 -nf UDM &
$B/mock-nf -addr 127.0.0.1:9102 -id udm-2 -nf UDM &
$B/mock-nf -addr 127.0.0.1:9201 -id amf-1 -nf AMF &
# the SCP
$B/nginx -p "$PWD/scp-poc" -c conf/scp.conf &

# a consumer request (h2c, Model D)
curl --http2-prior-knowledge \
  -H "3gpp-Sbi-Target-apiRoot: http://udm-set" \
  -H "Authorization: Bearer eyJ.test.token" \
  http://127.0.0.1:8080/nudm-sdm/v2/imsi-001/am-data
# -> {"nf":"UDM","instance":"udm-1","proto":"HTTP/2.0","path":"/nudm-sdm/v2/..."}
```

## Scope & limitations

This is a PoC. **Out of scope:** delegated NRF discovery (Model C/D discovery),
OAuth2 token *validation* (only presence is checked), binding-header reselection
semantics, overload control, TLS/mTLS to NFs, and SEPP/N32. See the roadmap in
[PITCH.md](PITCH.md).
