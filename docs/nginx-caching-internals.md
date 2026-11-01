# nginx Caching Internals — End-to-End Source Walkthrough

A source-grounded deep dive into how nginx's HTTP content cache works, from the
backend socket to the client, and why it is independent of the client-facing
protocol (HTTP/1.0, HTTP/1.1, HTTP/2, HTTP/3).

All file/line references point at this repository's source tree.

---

## Table of contents

1. [The big picture — where the cache lives and the protocol question](#1-the-big-picture)
2. [The caching engine data model](#2-data-model)
3. [Cache key and file naming](#3-cache-key-and-file-naming)
4. [Request flow and cache statuses](#4-request-flow-and-cache-statuses)
5. [Deciding if a response is cacheable, and freshness](#5-cacheability-and-freshness)
6. [Writing to cache: `ngx_http_file_cache_update` (temp-file → rename)](#6-writing-to-cache)
7. [The EXPIRED → REVALIDATED conditional-request path](#7-expired--revalidated)
8. [The Vary / variant secondary-key mechanism](#8-vary--variant)
9. [The cache-lock wait-timer state machine (`proxy_cache_lock`)](#9-cache-lock)
10. [The loader + manager LRU eviction loop](#10-loader--manager)
11. [The event pipe: `upstream_done` / `upstream_eof` / `upstream_error`](#11-event-pipe)
12. [`ngx_event_pipe_write_chain_to_temp_file` and shadow-buffer recycling](#12-write-chain-and-shadow-buffers)
13. [The rbtree key lookup (`ngx_http_file_cache_lookup`)](#13-rbtree-lookup)
14. [The slab allocator behind `keys_zone`](#14-slab-allocator)
15. [The non-buffered path (`proxy_buffering off`) and why it can't cache](#15-non-buffered-path)
16. [The send path: file buffers → `ngx_output_chain` → `sendfile`](#16-send-path)
17. [Async disk I/O: `p->writing` / `p->aio` and `aio threads`](#17-async-disk-io)
18. [A working `proxy_cache_path` + `proxy_cache` config with h2/h3](#18-example-config)
19. [End-to-end journey summary](#19-end-to-end-summary)

---

<a name="1-the-big-picture"></a>
## 1. The big picture — where the cache lives and the protocol question

The most important thing to understand: **nginx's content cache lives on the
*upstream* (backend-facing) side, and is independent of the *client-facing*
protocol (HTTP/1.0, HTTP/1.1, HTTP/2, HTTP/3).**

- The cache is driven by the **upstream machinery** in
  `src/http/ngx_http_upstream.c` plus the generic file-cache engine in
  `src/http/ngx_http_file_cache.c`. It is wired up by the proxying modules:
  `proxy`, `fastcgi`, `scgi`, `uwsgi` (each provides `*_cache` / `*_cache_key`
  directives). e.g. `ngx_http_proxy_create_key`
  (`src/http/modules/ngx_http_proxy_module.c:1085`).
- The **client-facing protocol handlers** — HTTP/1.x in `ngx_http_request.c`,
  HTTP/2 in `src/http/v2`, HTTP/3 in `src/http/v3` — only terminate the
  transport. A cached response is replayed through the normal output filter
  chain via `ngx_http_cache_send` (`src/http/ngx_http_cache.h:197`), so it is
  serialized to whatever protocol the client negotiated. **There is no HTTP/2-
  or HTTP/3-specific cache.** The same cache entry serves an h1, h2, or h3
  client identically.

So "caching for HTTP/1.1 vs HTTP/2 vs HTTP/3" is really: *the client can speak
any of those three; the cache doesn't know or care.* What matters is the
**backend** protocol:

| Direction | Protocols | Cache integration |
|---|---|---|
| Client → nginx | HTTP/1.0, HTTP/1.1, HTTP/2, HTTP/3 | Transparent — cache is protocol-agnostic; cached bytes go through output filters |
| nginx → backend | HTTP/1.0 & HTTP/1.1 (`proxy`), FastCGI, SCGI, uwsgi | **Fully cacheable** — these set `u->create_key` and `u->cacheable` |
| nginx → backend | gRPC (`ngx_http_grpc_module.c`) | **Not cacheable** — no `create_key`, no cache hookup |

---

<a name="2-data-model"></a>
## 2. The caching engine data model — three representations of one entry

Defined in `src/http/ngx_http_cache.h`:

- **`ngx_http_cache_t`** (`struct ngx_http_cache_s`, `ngx_http_cache.h:62`) — the
  per-request cache context (`r->cache`). Holds the key, crc32, validity times,
  etag/vary, the open file handle, lock state, etc.
- **`ngx_http_file_cache_node_t`** (`ngx_http_cache.h:36`) — the in-memory index
  entry. These live in an **rbtree + LRU queue inside a shared-memory slab zone**
  (`keys_zone=` in `proxy_cache_path`), so all worker processes share one view of
  what's cached. It stores a truncated key, use counts, expiry, size, and flags
  (`exists`, `updating`, `deleting`, `purged`).
- **`ngx_http_file_cache_header_t`** (`ngx_http_cache.h:122`) — the fixed header
  prepended to the **on-disk cache file**, followed by the `KEY: ...` line and
  then the stored HTTP response headers + body. `NGX_HTTP_CACHE_VERSION 5` guards
  the format.

Cache status constants (`ngx_http_cache.h:16`) and their user-visible strings
(`ngx_http_cache_status`, `ngx_http_file_cache.c:68`), surfaced as
`$upstream_cache_status`:

```
MISS  BYPASS  EXPIRED  STALE  UPDATING  REVALIDATED  HIT   (+ internal SCARCE)
```

---

<a name="3-cache-key-and-file-naming"></a>
## 3. Cache key and file naming

- The key is whatever `proxy_cache_key` (etc.) evaluates to.
  `ngx_http_file_cache_create_key` (`ngx_http_file_cache.c:228`) folds all key
  parts into **both an MD5 (16 bytes, the real identity) and a CRC32** (used as
  the rbtree node key / quick compare).
- The on-disk path is derived purely from the hex MD5 in
  `ngx_http_file_cache_name` (`ngx_http_file_cache.c:1015`): the 32-hex-char
  digest is laid down and `ngx_create_hashed_filename` splits off the trailing
  characters into subdirectories according to `levels=` (e.g.
  `/cache/c/29/b7f54b2df7773722d382f4809d650c`).

---

<a name="4-request-flow-and-cache-statuses"></a>
## 4. Request flow and cache statuses

Entry point `ngx_http_upstream_cache` (`ngx_http_upstream.c:875`), called from
the upstream init at `ngx_http_upstream.c:615`:

1. Check the request method against `*_cache_methods`; create `r->cache`, build
   the key.
2. Evaluate `proxy_cache_bypass` → if it fires, status **BYPASS** and go to the
   backend without reading cache.
3. Call `ngx_http_file_cache_open` (`ngx_http_file_cache.c:265`) →
   `ngx_http_file_cache_exists` (`ngx_http_file_cache.c:901`) does the rbtree
   lookup, then reads & validates the on-disk header.

Status meanings:

- **MISS** — not present; fetch and store.
- **HIT** — present and fresh; served directly from disk.
- **EXPIRED** — present but past `valid_sec`; revalidate/refetch.
- **STALE / UPDATING** — returned under `proxy_cache_use_stale` / background
  update.
- **REVALIDATED** — conditional revalidation (If-Modified-Since / If-None-Match)
  returned 304, so the stale copy is refreshed in place.
- **BYPASS** — `proxy_cache_bypass` matched.
- **SCARCE** — entry exists but hasn't met `proxy_cache_min_uses` yet, so it isn't
  served/stored yet.

---

<a name="5-cacheability-and-freshness"></a>
## 5. Deciding if a response is cacheable, and freshness

When backend headers arrive, `ngx_http_upstream_process_headers`
(`ngx_http_upstream.c:3095`) and friends set `u->cacheable = 0` if the backend
sent `no_cache`/`expired`. Freshness is computed from, in priority order:

- `X-Accel-Expires` (nginx-specific),
- `Cache-Control` — parsed in `ngx_http_upstream_process_cache_control`
  (`ngx_http_upstream.c:5102`), honoring `s-maxage=`, `max-age=`,
  `no-cache`/`no-store`/`private`, and `stale-while-revalidate=` /
  `stale-if-error=` (`ngx_http_upstream.c:5172`),
- `Expires` — `ngx_http_upstream_process_expires` (`ngx_http_upstream.c:5242`),
- falling back to the admin's `proxy_cache_valid` table via
  `ngx_http_file_cache_valid`.

`Vary` is captured by `ngx_http_upstream_process_vary`
(`ngx_http_upstream.c:5534`); see §8.

---

<a name="6-writing-to-cache"></a>
## 6. Writing to cache: `ngx_http_file_cache_update` (temp-file → rename)

A MISS response is streamed to a temporary file by the event pipe; when the
backend finishes, that temp file is atomically promoted into the cache and the
shared-memory index node is updated. Three stages.

### Stage A — reserve the header slot and write it

Before any body streaming, in `ngx_http_upstream_send_response`
(`ngx_http_upstream.c:3460`) nginx records the body offset and the validators a
later revalidation will need, then writes the fixed cache header into the front
of the upstream buffer:

```c
r->cache->body_start = (u_short) (u->buffer.pos - u->buffer.start);
...
r->cache->last_modified = u->headers_in.last_modified_time;   // only for 200/206
r->cache->etag = u->headers_in.etag->value;
...
ngx_http_file_cache_set_header(r, u->buffer.start);           // writes header+KEY line
```

`ngx_http_file_cache_set_header` (`ngx_http_file_cache.c:1324`) lays down
`ngx_http_file_cache_header_t` (version, crc32, `valid_sec`, etag, vary/variant)
followed by the `\nKEY: ...` line used for collision detection on read.

### Stage B — stream the body to a temp file

The event pipe is configured for persistent buffering
(`ngx_http_upstream.c:3517`):

```c
p->cacheable = u->cacheable || u->store;
p->temp_file->persistent = 1;
if (r->cache && !r->cache->file_cache->use_temp_path) {
    p->temp_file->path = r->cache->file_cache->path;   // temp lives INSIDE the cache tree
    p->temp_file->file.name = r->cache->file.name;     // so the rename is same-filesystem
}
```

This is why `use_temp_path=off` matters: the temp file is created under the cache
directory, so the final rename is a cheap same-device `rename()` rather than a
cross-filesystem copy.

### Stage C — promote on completion

When the pipe signals `upstream_done` (or a clean `upstream_eof` with a matching
Content-Length), `ngx_http_upstream_process_request`
(`ngx_http_upstream.c:4461`) calls the store:

```c
if (p->upstream_done) {
    ngx_http_file_cache_update(r, p->temp_file);
} else if (p->upstream_eof) {
    if (p->length == -1 && (content_length matches tf->offset - body_start)) {
        ngx_http_file_cache_update(r, tf);      // complete -> store
    } else {
        ngx_http_file_cache_free(r->cache, tf); // truncated -> discard
    }
} else if (p->upstream_error) {
    ngx_http_file_cache_free(r->cache, p->temp_file);
}
```

**Integrity gate:** a short/aborted response is sent to `free` (temp file
deleted), never stored. Only a provably complete body reaches `update`.

`ngx_http_file_cache_update` (`ngx_http_file_cache.c:1439`):

```c
if (c->updated) return;              // idempotent guard
c->updated = 1; c->updating = 0;

ext.create_path = 1;                 // mkdir the 1:2 level dirs on demand
ext.delete_file = 1;                 // remove temp on failure
rc = ngx_ext_rename_file(&tf->file.name, &c->file.name, &ext);   // atomic rename

if (rc == NGX_OK) {
    ngx_fd_info(tf->file.fd, &fi);
    uniq = ngx_file_uniq(&fi);                              // inode identity
    fs_size = (ngx_file_fs_size(&fi)+bsize-1)/bsize;        // size in FS blocks
}

ngx_shmtx_lock(&cache->shpool->mutex);
c->node->count--;
c->node->error = 0;
c->node->uniq = uniq;
c->node->body_start = c->body_start;
cache->sh->size += fs_size - c->node->fs_size;   // adjust accounted total
c->node->fs_size = fs_size;
if (rc == NGX_OK) c->node->exists = 1;           // <-- now visible as a HIT
c->node->updating = 0;                           // release the "updating" lock
ngx_shmtx_unlock(&cache->shpool->mutex);
```

Key properties:
- The entry becomes a servable HIT the instant `exists = 1` is set under the
  shared-memory mutex — a torn file is never observable because the rename is
  atomic and `exists` flips only after it succeeds.
- `cache->sh->size` is maintained in **filesystem blocks** (`bsize`), the
  quantity the manager compares against `max_size`.
- `c->node->updating = 0` here releases the cache-lock other requests may be
  waiting on (§9).

(The sibling `ngx_http_file_cache_update_header`, `ngx_http_file_cache.c:1514`,
is the REVALIDATED path's in-place header rewrite — no rename, no body — guarded
by `uniq`/`length`/`crc32` checks so it refuses to touch a file replaced
underneath it.)

---

<a name="7-expired--revalidated"></a>
## 7. The EXPIRED → REVALIDATED conditional-request path

When `proxy_cache_revalidate on;` and a cached entry has gone stale.

### Phase A — the hit is classified as EXPIRED

`ngx_http_file_cache_read` (`ngx_http_file_cache.c:547`) loads the on-disk header
and copies its revalidators into `r->cache` (`ngx_http_file_cache.c:627`):

```c
c->valid_sec     = h->valid_sec;
c->last_modified = h->last_modified;   // from stored Last-Modified
c->etag.len      = h->etag_len;        // from stored ETag
c->etag.data     = h->etag;
```

Freshness check at `ngx_http_file_cache.c:650`: `if (c->valid_sec < now)` → the
entry is stale. Back in `ngx_http_upstream_cache` this becomes
`u->cache_status = NGX_HTTP_CACHE_EXPIRED` (`ngx_http_upstream.c:1006`).

### Phase B — inject the conditional request headers

The `proxy` module's cache-header table rewrites two request headers *only on
this revalidation path* (`ngx_http_proxy_cache_headers`,
`ngx_http_proxy_module.c:790`):

```c
{ ngx_string("If-Modified-Since"), ngx_string("$upstream_cache_last_modified") },
{ ngx_string("If-None-Match"),     ngx_string("$upstream_cache_etag") },
```

Those variables are gated. `ngx_http_upstream_cache_last_modified`
(`ngx_http_upstream.c:6351`) returns `not_found` (header omitted) unless:

```c
if (r->upstream == NULL
    || !r->upstream->conf->cache_revalidate                 // revalidate must be ON
    || r->upstream->cache_status != NGX_HTTP_CACHE_EXPIRED   // only for EXPIRED
    || r->cache->last_modified == -1)                        // we must have a validator
```

`ngx_http_upstream_cache_etag` (`ngx_http_upstream.c:6381`) has the identical
guard against `r->cache->etag.len == 0`.

### Phase C — handle the backend's 304 and refresh in place

`ngx_http_upstream.c:2875`:

```c
if (status == NGX_HTTP_NOT_MODIFIED
    && u->cache_status == NGX_HTTP_CACHE_EXPIRED
    && u->conf->cache_revalidate)
{
    now = ngx_time();
    valid    = r->cache->valid_sec;      // snapshot the OLD validity times
    updating = r->cache->updating_sec;
    error    = r->cache->error_sec;

    rc = u->reinit_request(r);           // reset upstream parse state

    u->cache_status = NGX_HTTP_CACHE_REVALIDATED;
    rc = ngx_http_upstream_cache_send(r, u);   // serve the EXISTING cached body
    ...
    if (valid) {
        r->cache->valid_sec    = valid;
        r->cache->updating_sec = updating;
        r->cache->error_sec    = error;
        r->cache->date         = now;
        ngx_http_file_cache_update_header(r);   // rewrites ONLY the on-disk header
    }
}
```

- **The body is never re-downloaded** — `ngx_http_upstream_cache_send` replays the
  stored body from disk. The 304 carried no body.
- **New expiry** is recomputed from a fresh `Cache-Control`/`Expires` on the 304,
  or from `proxy_cache_valid` for the 304's status (`ngx_http_upstream.c:2912`).
- `ngx_http_file_cache_update_header` (`ngx_http_file_cache.c:1514`) seeks to the
  front of the existing cache file and overwrites the fixed-size header — no temp
  file, no rename, no body rewrite.

Net effect: a stale entry whose content is unchanged costs one header-only
backend round-trip plus a tiny in-place header write.

Related sibling path (`ngx_http_upstream.c:2840`): if the backend *errors* while
the status is EXPIRED and `proxy_cache_use_stale` permits it, nginx flips to
`NGX_HTTP_CACHE_STALE` and serves the old body instead ("stale-if-error").

---

<a name="8-vary--variant"></a>
## 8. The Vary / variant secondary-key mechanism

`Vary` maps one logical URL to several stored representations via a **two-level
key: a primary key → a per-variant secondary key**.

### Storing: capture `Vary` and compute the variant hash

`ngx_http_upstream_process_vary` (`ngx_http_upstream.c:5534`):

- `Vary: *` ⇒ **uncacheable** (`u->cacheable = 0`, `ngx_http_upstream.c:5563`).
- Multiple `Vary` headers are concatenated; if combined length exceeds
  `NGX_HTTP_CACHE_VARY_LEN` (128) it's dropped as uncacheable.
- Otherwise saved into `r->cache->vary` (`ngx_http_upstream.c:5603`).

At write time, `ngx_http_file_cache_set_header` (`ngx_http_file_cache.c:1356`)
stores the `vary` text in the file header **and** computes the variant hash from
the *request's* values of those headers:

```c
h->vary_len = (u_char) c->vary.len;
ngx_memcpy(h->vary, c->vary.data, c->vary.len);
ngx_http_file_cache_vary(r, c->vary.data, c->vary.len, c->variant);  // -> hash
ngx_memcpy(h->variant, c->variant, NGX_HTTP_CACHE_KEY_LEN);
```

`ngx_http_file_cache_vary` (`ngx_http_file_cache.c:1137`) MD5s:
1. `r->cache->main` — the **primary** 16-byte key, then
2. for each header name in `Vary`: `name:` + request value(s) + CRLF.

`ngx_http_file_cache_vary_header` (`ngx_http_file_cache.c:1186`) pulls the
request-side values, and for `Accept-Charset` / `Accept-Encoding` /
`Accept-Language` it **normalizes whitespace/commas**.

### Reading: detect a variant mismatch and re-open

After loading the header, `ngx_http_file_cache_read` (`ngx_http_file_cache.c:615`)
recomputes the variant hash and compares:

```c
if (h->vary_len) {
    ngx_http_file_cache_vary(r, h->vary, h->vary_len, c->variant);
    if (ngx_memcmp(c->variant, h->variant, NGX_HTTP_CACHE_KEY_LEN) != 0) {
        return ngx_http_file_cache_reopen(r, c);   // wrong variant
    }
}
```

`ngx_http_file_cache_reopen` (`ngx_http_file_cache.c:1289`) does the second-level
lookup:

```c
c->secondary = 1;
c->file.name.len = 0;
ngx_memcpy(c->key, c->variant, NGX_HTTP_CACHE_KEY_LEN);  // key = variant hash
return ngx_http_file_cache_open(r);                      // look up variant file
```

The **primary-key file acts as a pointer** recording which `Vary` headers matter;
the actual representation lives in a **variant-key file**. The `c->secondary`
flag prevents infinite recursion — a second mismatch is a hard error
(`ngx_http_file_cache.c:1298`). `ngx_http_file_cache_update_variant`
(`ngx_http_file_cache.c:1388`) keeps the pointer accurate.

| | Primary key | Variant (secondary) key |
|---|---|---|
| Source | `MD5(proxy_cache_key)` → `c->main` | `MD5(main ‖ Vary-header request values)` |
| On-disk role | records *which* request headers vary | holds the actual stored response |
| Built by | `create_key` (`ngx_http_file_cache.c:228`) | `ngx_http_file_cache_vary` (`ngx_http_file_cache.c:1137`) |

---

<a name="9-cache-lock"></a>
## 9. The cache-lock wait-timer state machine (`proxy_cache_lock`)

On a cold key, let exactly one request populate the entry while the rest wait.
State lives in the shared node (`updating`, `lock_time`) and in the per-request
`c` (`waiting`, `wait_time`, `wait_event`).

### Entry: acquire-or-wait — `ngx_http_file_cache_lock` (`ngx_http_file_cache.c:404`)

```c
timer = c->node->lock_time - now;
if (!c->node->updating || (ngx_msec_int_t) timer <= 0) {
    c->node->updating = 1;                     // I become the updater
    c->node->lock_time = now + c->lock_age;    // my lease expires in lock_age
    c->updating = 1;
}
```

A request becomes **the updater** if no one is updating, *or* the previous
updater's `lock_age` lease expired (crash/stall recovery —
`proxy_cache_lock_age`). Outcomes:

- `c->updating == 1` → `NGX_DECLINED`: **go to the backend** to fill the entry.
- else if `lock_timeout == 0` or too many blocked subrequests
  (`NGX_HTTP_MAX_BLOCKED`) → `NGX_HTTP_CACHE_SCARCE`: **give up waiting, go to
  backend uncached**.
- else → become a **waiter**:

```c
c->waiting = 1;
if (c->wait_time == 0) {
    c->wait_time = now + c->lock_timeout;              // overall deadline
    c->wait_event.handler = ngx_http_file_cache_lock_wait_handler;
    c->wait_event.data = r;
}
timer = c->wait_time - now;
ngx_add_timer(&c->wait_event, (timer > 500) ? 500 : timer);  // poll at most every 500ms
r->main->blocked++;
return NGX_AGAIN;    // request parked
```

The **500 ms cap** means a waiter re-checks at least twice a second, waking
promptly once the updater finishes.

### Tick: `ngx_http_file_cache_lock_wait_handler` (`ngx_http_file_cache.c:467`)

Delegates to `ngx_http_file_cache_lock_wait` (`ngx_http_file_cache.c:506`):

```c
timer = c->wait_time - now;
if ((ngx_msec_int_t) timer <= 0) {          // overall deadline hit
    "cache lock timeout";
    c->lock_timeout = 0;                     // mark timed-out
    return NGX_OK;                           // stop waiting
}
timer = c->node->lock_time - now;
if (c->node->updating && (ngx_msec_int_t) timer > 0) {
    wait = 1;                                // updater still alive -> keep waiting
}
if (wait) { ngx_add_timer(&c->wait_event, min(timer,500)); return NGX_AGAIN; }
return NGX_OK;                               // updater done (or lease expired) -> proceed
```

On non-AGAIN it clears `waiting`, decrements `blocked`, and re-runs the request
(`r->write_event_handler(r)` + `ngx_http_run_posted_requests`). The request
re-enters `ngx_http_file_cache_open`: HIT if stored; SCARCE → backend if timed
out.

### Exit: releasing the lock

The updater clears `node->updating` on success in `ngx_http_file_cache_update`
(`ngx_http_file_cache.c:1508`) or on failure in `ngx_http_file_cache_free`
(`ngx_http_file_cache.c:1714`):

```c
if (c->updating && fcn->lock_time == c->lock_time) {  // only if I still hold the lease
    fcn->updating = 0;
}
```

The `lock_time == c->lock_time` check ensures a request whose lease expired and
was taken over doesn't clobber the new updater's lock.

```
   open() miss
       │
       ▼
   lock(): node->updating?
    ┌───┴───────────────┐
    │no / lease expired  │yes
    ▼                    ▼
 become updater      waiter: add 500ms timer, NGX_AGAIN
 (DECLINED→backend)      │
    │                    ▼ (tick) wait_handler → wait()
    ▼                 deadline? ──yes──► lock_timeout=0 → SCARCE → backend
 update()/free():     updater still updating? ──yes──► re-arm 500ms
 node->updating=0  ◄──────── no ───────► rerun request → HIT
```

---

<a name="10-loader--manager"></a>
## 10. The loader + manager LRU eviction loop

The **LRU order is the shared `cache->sh->queue`**: head = most-recently-touched,
tail = eviction candidate. Every lookup bumps the entry to the head; the manager
evicts from the tail.

### Maintaining LRU order

On **every access**, `ngx_http_file_cache_exists` (`ngx_http_file_cache.c:901`)
removes the node from the queue, then at `done:` reinserts at the head with a
refreshed idle deadline:

```c
fcn->expire = ngx_time() + cache->inactive;
ngx_queue_insert_head(&cache->sh->queue, &fcn->queue);
```

### The loader (startup warm-up) — `ngx_http_file_cache_loader` (`ngx_http_file_cache.c:2130`)

Runs once while the zone is `cold`, guarded by an atomic:

```c
if (!cache->sh->cold || cache->sh->loading) return;
if (!ngx_atomic_cmp_set(&cache->sh->loading, 0, ngx_pid)) return;
```

Walks the on-disk tree (`ngx_walk_tree`); for each file
`ngx_http_file_cache_manage_file` (`ngx_http_file_cache.c:2184`) calls `add_file`
to rebuild the rbtree/queue index. Self-throttles with
`loader_files`/`loader_threshold`/`loader_sleep`. When done it flips
`sh->cold = 0`.

### The manager (ongoing eviction) — `ngx_http_file_cache_manager` (`ngx_http_file_cache.c:2042`)

**Job 1 — inactivity expiry**, via `ngx_http_file_cache_expire`
(`ngx_http_file_cache.c:1880`):

```c
q = ngx_queue_last(&cache->sh->queue);        // oldest / LRU tail
fcn = ngx_queue_data(q, ...);
wait = fcn->expire - now;
if (wait > 0) { wait = min(wait,10); break; } // tail not idle yet -> nothing to do
if (fcn->count == 0) { ngx_http_file_cache_delete(cache, q, name); goto next; }
if (fcn->deleting) { wait = 1; break; }
// in-use & stuck: requeue to head with a fresh inactive deadline, log, continue
```

**Job 2 — size/space enforcement** (`max_size` / `min_free`):

```c
size  = cache->sh->size;   count = cache->sh->count;   watermark = cache->sh->watermark;
if (size < cache->max_size && count < watermark) {
    if (!cache->min_free) break;                       // nothing to reclaim
    free = ngx_fs_available(path);
    if (free > cache->min_free) break;
}
wait = ngx_http_file_cache_forced_expire(cache);       // force-evict LRU tail
if (wait > 0) { next = wait*1000; break; }
```

`ngx_http_file_cache_forced_expire` (`ngx_http_file_cache.c:1781`) deletes from
the LRU tail *ignoring* `inactive`/`expire` (up to 20 tries per pass) to get back
under budget.

**The watermark** (`ngx_http_file_cache_set_watermark`,
`ngx_http_file_cache.c:2364`) adds a *node-count* budget:

```c
cache->sh->watermark = cache->sh->count - cache->sh->count / 8;   // ~87.5% of current count
```

It is set from `ngx_http_file_cache_exists` (`ngx_http_file_cache.c:954`) when a
slab allocation for a new node **fails** (keys_zone exhausted) — so keys_zone
pressure, not just disk size, drives eviction.

### The actual delete — `ngx_http_file_cache_delete` (`ngx_http_file_cache.c:1991`)

```c
if (fcn->exists) {
    cache->sh->size -= fcn->fs_size;         // give back accounted blocks
    fcn->count++; fcn->deleting = 1;
    ngx_shmtx_unlock(...);                    // drop mutex for the slow unlink()
    ngx_delete_file(name);
    ngx_shmtx_lock(...);
    fcn->count--; fcn->deleting = 0;
}
if (fcn->count == 0) {                        // no in-flight refs
    ngx_queue_remove(q);
    ngx_rbtree_delete(&cache->sh->rbtree, &fcn->node);
    ngx_slab_free_locked(cache->shpool, fcn); // free the index node
    cache->sh->count--;
}
```

The mutex is **released around the filesystem `unlink()`** (marking `deleting=1`)
to keep shared-memory lock hold time tiny.

---

<a name="11-event-pipe"></a>
## 11. The event pipe: `upstream_done` / `upstream_eof` / `upstream_error`

The buffered proxy path uses `ngx_event_pipe`, a producer/consumer engine moving
bytes **upstream → (buffers) → downstream and/or temp file**.

### The outer loop — `ngx_event_pipe` (`ngx_event_pipe.c:23`)

```c
for ( ;; ) {
    if (do_write) {
        rc = ngx_event_pipe_write_to_downstream(p);   // drain to client (+temp file)
        if (rc == NGX_ABORT) return NGX_ABORT;
        if (rc == NGX_BUSY)  return NGX_OK;
    }
    p->read = 0;
    p->upstream_blocked = 0;
    if (ngx_event_pipe_read_upstream(p) == NGX_ABORT) return NGX_ABORT;
    if (!p->read && !p->upstream_blocked) break;       // nothing happened -> done for now
    do_write = 1;
}
```

### The producer — `ngx_event_pipe_read_upstream` (`ngx_event_pipe.c:103`)

Buffer-sourcing policy (`ngx_event_pipe.c:221`) is the heart of flow control:

1. `p->free_raw_bufs` — reuse a freed buffer.
2. else if `p->allocated < p->bufs.num` — allocate a new one (`proxy_buffers`).
3. else if **not cacheable** and the client is ready — `p->upstream_blocked = 1`
   and break, forcing a downstream write (backpressure to client).
4. else if `p->cacheable || temp_file->offset < max_temp_file_size` — **spill to
   the temp file** via `ngx_event_pipe_write_chain_to_temp_file`, freeing buffers.
5. else — no buffers available, break.

For a **cacheable** response branch 3 never fires, so when `proxy_buffers` fill
data is always pushed to the temp file (branch 4).

Terminal-flag logic from the socket:

```c
n = p->upstream->recv_chain(p->upstream, chain, limit);
...
if (n == NGX_ERROR) { p->upstream_error = 1; break; }   // socket error
if (n == NGX_AGAIN) { break; }                          // would block, try later
p->read = 1;
if (n == 0) { p->upstream_eof = 1; break; }             // peer closed connection
```

After the loop: `if (p->length == 0) { p->upstream_done = 1; p->read = 1; }`.

### `p->length` and `upstream_done`

Seeded by `ngx_http_proxy_input_filter_init` (`ngx_http_proxy_module.c:2067`):

| Response framing | `p->pipe->length` init | filter |
|---|---|---|
| 204 / 304 / HEAD / Content-Length:0 | `0` (no body) | — |
| `Content-Length: N` | `N` | copy filter |
| `Transfer-Encoding: chunked` | `5` (min for `"0"CRLFCRLF`) | `ngx_http_proxy_chunked_filter` |
| connection-close framing | `-1` (until EOF) | copy filter |

**Content-Length** (`ngx_http_proxy_copy_filter`, `ngx_http_proxy_module.c:2126`):

```c
if (p->length == -1) return NGX_OK;             // close-framed: never completes by length
if (b->last - b->pos > p->length) {             // backend sent too much
    b->last = b->pos + p->length;               // trim to declared length
    p->upstream_done = 1;
}
p->length -= b->last - b->pos;
```

**Chunked** (`ngx_http_proxy_chunked_filter`, `ngx_http_proxy_module.c:2203`): on
the final chunk (`rc == NGX_DONE`, `ngx_http_proxy_module.c:2329`) sets
`p->length = 0` → `upstream_done`. Invalid chunk → `NGX_ERROR` → pipe aborts.

### Consumer semantics

- **`upstream_done`** = framing says "complete" → always safe to cache.
- **`upstream_eof`** = FD closed. Only trustworthy if the response was defined by
  connection-close (`length == -1`) and the byte count matches; otherwise
  truncated → must **not** be cached.
- **`upstream_error`** = never cache.

This is why a backend that drops mid-stream never leaves a corrupt cache file.

---

<a name="12-write-chain-and-shadow-buffers"></a>
## 12. `ngx_event_pipe_write_chain_to_temp_file` and shadow-buffer recycling

`ngx_event_pipe_write_chain_to_temp_file` (`ngx_event_pipe.c:728`) writes the
in-memory input chain `p->in` to the temp file and turns written regions into
**file-backed buffers on `p->out`**.

### Choosing what to write — the cacheable split (`ngx_event_pipe.c:773`)

```c
if (!p->cacheable) {
    // walk p->in accumulating size, stop at a shadow boundary when either
    //   size + bsize > temp_file_write_size          (write granularity)
    //   offset + size + bsize > max_temp_file_size    (hard cap)
    ...
    if (ll == NULL) return NGX_BUSY;   // nothing writable yet
} else {
    p->in = NULL;             // CACHEABLE: write the ENTIRE input chain, unconditionally
    p->last_in = &p->in;
}
```

Asymmetry: for a **cacheable** response the whole `p->in` is written with **no
`max_temp_file_size`/`temp_file_write_size` throttling** — the cache file must
contain the complete body. (`proxy_max_temp_file_size` only bounds the
*non-cacheable* buffering-to-disk case.)

### Writing and building output file-buffers (`ngx_event_pipe.c:835`)

```c
n = ngx_write_chain_to_temp_file(p->temp_file, out);   // the actual writev to disk
...
if (n > 0) {
    if (p->out) {   // can we just extend the last file buffer?
        b = <last of p->out>;
        if (b->file_last == p->temp_file->offset) {     // contiguous on disk
            p->temp_file->offset += n;
            b->file_last = p->temp_file->offset;         // coalesce, no new buf
            goto free;
        }
    }
    cl = ngx_chain_get_free_buf(p->pool, &p->free);
    b = cl->buf; ngx_memzero(b, ...);
    b->file = &p->temp_file->file;
    b->file_pos  = p->temp_file->offset;
    p->temp_file->offset += n;
    b->file_last = p->temp_file->offset;
    b->in_file = 1; b->temp_file = 1;                    // a file-backed buffer
    *last_out = cl;                                      // appended to p->out
}
```

Data that lived in RAM is now a descriptor `{file, file_pos, file_last}` — no
payload bytes, just an offset range. Contiguous writes coalesce.

### Releasing RAM buffers — the `free:` label (`ngx_event_pipe.c:907`)

```c
for (cl = out; cl; cl = next) {
    next = cl->next;
    cl->next = p->free; p->free = cl;          // return chain link to p->free
    b = cl->buf;
    if (b->last_shadow) {                       // this was the LAST data buf of a raw read buf
        tl = ngx_alloc_chain_link(p->pool);
        tl->buf = b->shadow;                    // the underlying raw read buffer
        *last_free = tl; last_free = &tl->next;
        b->shadow->pos = b->shadow->last = b->shadow->start;  // reset it empty
        ngx_event_pipe_remove_shadow_links(b->shadow);        // sever the shadow chain
    }
}
```

### Shadow-buffer mechanics

nginx uses **two buffer views over one block of memory**:

- a **raw buffer** (`p->free_raw_bufs`, size = `proxy_buffer_size`/`proxy_buffers`)
  that `recv_chain` reads bytes into;
- one or more **shadow buffers** (`p->in`) overlaying the *same* memory to mark
  logical data regions (e.g. individual dechunked segments inside one raw read).

They point at each other via `buf->shadow`; the final shadow in a group carries
`last_shadow = 1`. A raw read buffer is **pinned** until all its shadows have been
consumed (sent downstream or written to temp file).

Chains:

| Chain | Holds | Meaning |
|---|---|---|
| `p->free_raw_bufs` | raw buffers | ready to `recv` into |
| `p->in` | shadow (data) buffers | received, not yet consumed |
| `p->out` | file buffers | written to temp file, pending send |
| `p->busy` | buffers handed to output filter | sent but not yet fully flushed (recycled) |
| `p->free` | spent chain links | reusable `ngx_chain_t` |

Three recycling paths:

- **(a) Via temp file** — shown above: freeing a `last_shadow` shadow resets its
  raw buf and pushes to `free_raw_bufs`.
- **(b) Via downstream send** — `ngx_event_pipe_write_to_downstream`
  (`ngx_event_pipe.c:492`) builds `out` from `p->out` (and, **only when
  `!p->cacheable`**, directly from `p->in` — `ngx_event_pipe.c:627`), calls
  `p->output_filter`, then `ngx_chain_update_chains` moves finished buffers to
  `p->free`, and recycles their shadow raw bufs. The cacheable guard guarantees
  the cache file is written before/as the client is served.
- **(c) `recycled` + `busy_size` backpressure** — recycled buffers count toward
  `bsize`; when `bsize >= p->busy_size` (`ngx_event_pipe.c:616`) the pipe forces a
  flush to return memory promptly.

Helpers: `ngx_event_pipe_remove_shadow_links` (`ngx_event_pipe.c:1015`) severs
the shadow ring; `ngx_event_pipe_add_free_buf` (`ngx_event_pipe.c:1047`) resets a
raw buffer and reinserts it preserving read locality.

```
 recv_chain → raw buf ─shadow→ p->in (data bufs)
                           │
         ┌─────────────────┴─────────────────┐
    cacheable?                           not cacheable
         │                                     │
  write_chain_to_temp_file            send p->in directly
   → p->out (file bufs) → client        → client
         │                                     │
     last_shadow buf freed ──► reset raw buf ──► free_raw_bufs (reused)
```

---

<a name="13-rbtree-lookup"></a>
## 13. The rbtree key lookup — `ngx_http_file_cache_lookup` (`ngx_http_file_cache.c:1049`)

The cache key is a 16-byte MD5. The rbtree node key (`ngx_rbtree_key_t`) is only
the machine word width (8 bytes on 64-bit), so nginx splits the MD5:

- **first `sizeof(ngx_rbtree_key_t)` bytes** → `node->key` (ordering key);
- **remaining `16 - sizeof(key)` bytes** → `fcn->key[]` (a `memcmp` tiebreaker).

```c
ngx_memcpy(&node_key, key, sizeof(ngx_rbtree_key_t));   // first 8 bytes
node = cache->sh->rbtree.root;
while (node != sentinel) {
    if (node_key < node->key) { node = node->left;  continue; }
    if (node_key > node->key) { node = node->right; continue; }
    /* first 8 bytes equal -> compare the remaining 8 */
    fcn = (ngx_http_file_cache_node_t *) node;
    rc = ngx_memcmp(&key[sizeof(ngx_rbtree_key_t)], fcn->key,
                    NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));
    if (rc == 0) return fcn;                              // full 16-byte match
    node = (rc < 0) ? node->left : node->right;           // tiebreak descends
}
return NULL;
```

- **The tiebreaker drives tree descent too.** `ngx_http_file_cache_rbtree_insert_value`
  (`ngx_http_file_cache.c:1094`) uses the identical comparison when inserting, so
  the low-8-byte ordering is a valid secondary sort.
- **A full 16-byte match is still only "probably" the right object.** The stored
  file begins with a `\nKEY: <plaintext key>\n` line, and
  `ngx_http_file_cache_read` (`ngx_http_file_cache.c:586`) `memcmp`s the live key
  parts against it, logging `"md5 collision"` and returning `NGX_DECLINED` on
  mismatch. The CRC32 in the file header is a further cross-check.

The caller `ngx_http_file_cache_exists` runs this under `shpool->mutex`, and on a
hit does `ngx_queue_remove` + `count++`/`uses++` (LRU bump + pin). Lookups are
O(log n) over the shared rbtree.

---

<a name="14-slab-allocator"></a>
## 14. The slab allocator behind `keys_zone`

`proxy_cache_path ... keys_zone=NAME:SIZE` carves a shared-memory segment whose
front is an `ngx_slab_pool_t`. Every `ngx_http_file_cache_node_t` is allocated
with `ngx_slab_calloc_locked`.

### Memory layout — `ngx_slab_init` (`src/core/ngx_slab.c:99`)

After the pool header, three regions:

1. **slot array** — `n = pagesize_shift - min_shift` list heads, one per size
   class.
2. **stats array** — `n` `ngx_slab_stat_t` (reqs/used/fails/total per class).
3. **page-descriptor array** (`pool->pages`) + the **actual data pages**
   (`pool->start .. pool->end`), page-aligned.

`pool->free` is a circular list of runs of contiguous free pages.

### Size classes — `ngx_slab_sizes_init` (`src/core/ngx_slab.c:86`)

```c
ngx_slab_max_size   = pagesize / 2;                      // 2048 on 4K pages
ngx_slab_exact_size = pagesize / (8 * sizeof(uintptr_t)); // 64 on 64-bit/4K
```

| Regime | Size range | Free map |
|---|---|---|
| **page** | `> max_size` (≥ 2048) | whole pages; `page->slab = npages \| PAGE_START` |
| **big** (`shift > exact_shift`) | exact_size < s ≤ max_size | bitmap in high bits of `page->slab` |
| **exact** (`shift == exact_shift`) | == exact_size (64) | `page->slab` is a 64-bit bitmap |
| **small** (`shift < exact_shift`) | < exact_size | bitmap at the start of the data page |

Sizes are rounded up to the next power of two (`shift`); `slot = shift -
min_shift`. A ~120-byte cache node lands in the **big** regime (rounded to 128).

### Allocation — `ngx_slab_alloc_locked` (`src/core/ngx_slab.c:184`)

Always under `shpool->mutex`. Phase 1: try an existing partial page of this slot
(`ngx_slab.c:227`); scan its free map for a 0 bit. Phase 2: no partial page →
grab a fresh one (`ngx_slab.c:331`) via `ngx_slab_alloc_pages`
(`ngx_slab.c:677`).

**On exhaustion** (`ngx_slab.c:420`): `ngx_slab_alloc_pages` returns NULL, so
`p = 0`, `stats[slot].fails++`, returns NULL. That NULL is what
`ngx_http_file_cache_exists` (`ngx_http_file_cache.c:949`) sees:

```c
fcn = ngx_slab_calloc_locked(cache->shpool, sizeof(ngx_http_file_cache_node_t));
if (fcn == NULL) {
    ngx_http_file_cache_set_watermark(cache);   // lower node-count budget to ~87.5%
    ngx_shmtx_unlock(...);
    (void) ngx_http_file_cache_forced_expire(cache);  // evict LRU tail to free nodes
    ngx_shmtx_lock(...);
    fcn = ngx_slab_calloc_locked(...);          // retry once
    if (fcn == NULL) { "could not allocate node"; return NGX_ERROR; }
}
```

**This links the two subsystems:** `keys_zone` sizing is a *node-count* limit
independent of `max_size` (the disk-byte limit). A too-small `keys_zone` triggers
constant forced eviction even when disk is nearly empty. Rough rule: `10m` holds
on the order of 80k entries.

### Freeing — `ngx_slab_free_locked`

Called from `ngx_http_file_cache_delete` (`ngx_http_file_cache.c:2037`) when a
node's `count` hits 0: identify regime from `page->prev` low bits, clear the
chunk's map bit, coalesce free page runs via `ngx_slab_free_pages`
(`ngx_slab.c:600`), `stats[slot].used--`.

---

<a name="15-non-buffered-path"></a>
## 15. The non-buffered path (`proxy_buffering off`) and why it can't cache

`u->buffering` is initialized straight from config in `ngx_http_proxy_handler`
(`ngx_http_proxy_module.c:946`): `u->buffering = plcf->upstream.buffering;`
(optionally flipped by `X-Accel-Buffering` via `ngx_http_upstream_process_buffering`,
`ngx_http_upstream.c:5408`, if `proxy_buffering` was on). nginx **never forces it
back on for caching.**

In `ngx_http_upstream_send_response` (`ngx_http_upstream.c:3347`) the branch order
is decisive:

```c
if (!u->buffering) {
#if (NGX_HTTP_CACHE)
    if (r->cache) {
        ngx_http_file_cache_free(r->cache, u->pipe->temp_file);   // <-- discard the cache entry
    }
#endif
    if (u->input_filter == NULL) {
        u->input_filter      = ngx_http_upstream_non_buffered_filter;
        ...
    }
    u->read_event_handler  = ngx_http_upstream_process_non_buffered_upstream;
    r->write_event_handler = ngx_http_upstream_process_non_buffered_downstream;
    ...
    return;     // <-- returns BEFORE the cacheable store block (line 3443+)
}
/* only buffered responses fall through to set_header / event_pipe / cache store */
```

So with buffering off:
1. `u->cacheable` may have been set to 1 earlier in `ngx_http_upstream_cache`
   (`ngx_http_upstream.c:922`) (so a *hit* can still be served from an existing
   file), but
2. on a **miss**, the moment the response starts, `ngx_http_file_cache_free`
   releases the reserved node/temp file and the function **returns before**
   `ngx_http_file_cache_set_header` / event-pipe setup /
   `ngx_http_file_cache_update` — none of the store machinery runs.

### The non-buffered loop — `ngx_http_upstream_process_non_buffered_request` (`ngx_http_upstream.c:3956`)

Uses a **single ping-pong buffer** `u->buffer` instead of a pool of pipe buffers
and a temp file:

```c
for ( ;; ) {
    if (do_write) {
        if (u->out_bufs || u->busy_bufs || downstream->buffered)
            ngx_http_output_filter(r, u->out_bufs);        // push what we have to client
        ngx_chain_update_chains(..., &u->free_bufs, &u->busy_bufs, &u->out_bufs, ...);
        if (u->busy_bufs == NULL) {
            if (u->length == 0 || (eof && u->length == -1)) { finalize(0); return; }
            if (upstream->read->eof) { "prematurely closed"; finalize(BAD_GATEWAY); return; }
            b->pos = b->start; b->last = b->start;          // RESET the one buffer
        }
    }
    size = b->end - b->last;
    if (size && upstream->read->ready) {
        n = upstream->recv(upstream, b->last, size);
        if (n > 0) u->input_filter(u->input_filter_ctx, n); // mark [b->last, b->last+n) as out buf
        do_write = 1; continue;
    }
    break;
}
```

The buffer can only be **reset and refilled once the client has drained it**
(`u->busy_bufs == NULL`). The backend is rate-limited by the client (true
streaming / backpressure), and **no complete copy of the body ever exists in one
place** — not in RAM, not on disk — so there is nothing to atomically rename into
the cache.

Related cases that also free the cache for the same reason:
- **Upgraded/WebSocket** connections (`ngx_http_upstream.c:3307`):
  `ngx_http_file_cache_free` then `ngx_http_upstream_upgrade`.
- `proxy_cache_bypass` that later becomes cacheable re-creates the file via
  `ngx_http_file_cache_create` (`ngx_http_upstream.c:3433`) — but only on the
  buffered path.

**Practical corollary:** `proxy_cache` + `proxy_buffering off` silently yields
**no cache writes** (hits from a pre-existing entry still work; misses never
store). To cache, buffering must be on — and because cacheable writes ignore
`max_temp_file_size` (§12), the whole body is always spooled regardless of size.

---

<a name="16-send-path"></a>
## 16. The send path: file buffers → `ngx_output_chain` → `sendfile`

The pipe produced `p->out` as **file-backed descriptors**
`{file=&temp_file->file, file_pos, file_last, in_file=1}`.
`ngx_event_pipe_write_to_downstream` hands these to `p->output_filter`, which is
`ngx_http_upstream_output_filter` (`ngx_http_upstream.c:4295`):

```c
static ngx_int_t
ngx_http_upstream_output_filter(void *data, ngx_chain_t *chain)
{
    r = data;  p = r->upstream->pipe;
    rc = ngx_http_output_filter(r, chain);   // the FULL HTTP body filter chain
    p->aio = r->aio;                         // propagate "async in progress" back to the pipe
    return rc;
}
```

The file buffers enter the **standard response body filter chain** (gzip, SSI,
sub_filter, range, and — crucially — **HTTP/2 or HTTP/3 framing**). The cache
stores the *origin* response body once; the per-client protocol layer re-frames
it at send time (protocol independence).

### `ngx_output_chain` — the copy filter's engine (`src/core/ngx_output_chain.c:42`)

For each buffer it asks `ngx_output_chain_as_is` (`ngx_output_chain.c:249`):

```c
sendfile = ctx->sendfile;
if (buf->in_file && buf->file->directio) sendfile = 0;   // directio defeats sendfile
if (!sendfile) {
    if (!ngx_buf_in_memory(buf)) return 0;   // must read file into RAM -> NOT as-is
    buf->in_file = 0;
}
if (ctx->need_in_memory && !ngx_buf_in_memory(buf)) return 0;  // e.g. gzip needs bytes in RAM
if (ctx->need_in_temp && (buf->memory || buf->mmap)) return 0;
return 1;
```

Two outcomes:

- **sendfile enabled, no in-memory-requiring filter downstream** → `as_is`
  returns 1; the `{file, file_pos, file_last}` descriptor passes through
  **unchanged** to the write filter. No userspace copy of the body — the kernel
  streams directly from the cache file fd to the client socket via `sendfile(2)`
  (`c->send_chain` → e.g. `ngx_linux_sendfile_chain`). Hot path for a HIT with
  `sendfile on`.
- **a filter needs bytes in memory** (gzip/SSI/sub_filter, or `sendfile off`, or
  `file_pos >= NGX_SENDFILE_LIMIT` on 32-bit) → `as_is` returns 0, and
  `ngx_output_chain_copy_buf` (`ngx_output_chain.c:504`) reads the file region
  into a reusable memory buffer (sized from `output_buffers`) with
  `ngx_read_file`. Even here, only `output_buffers`-sized windows are in RAM at a
  time, never the whole body.

Chain: **temp/cache file → (as-is descriptor | windowed read) → body filters →
write filter → `send_chain`/sendfile → client (framed as h1/h2/h3)**.

---

<a name="17-async-disk-io"></a>
## 17. Async disk I/O: `p->writing` / `p->aio` and `aio threads`

nginx must never block a worker on disk I/O. Both the **write** side (spooling to
the temp/cache file) and the **read** side (reading the cache file back for
`copy_buf`) can be offloaded to a thread pool (`aio threads`) or kernel AIO. Flags
on the pipe: `p->writing` (a temp-file write is outstanding) and `p->aio` (any
async op is in flight).

### Write offload — threaded `write_chain_to_temp_file`

`ngx_http_upstream_send_response` (`ngx_http_upstream.c:3551`) sets
`p->thread_handler = ngx_http_upstream_thread_handler`. Then in
`ngx_event_pipe_write_chain_to_temp_file` (`ngx_event_pipe.c:828`):

```c
if (p->thread_handler) {
    p->temp_file->thread_write = 1;
    p->temp_file->file.thread_task = p->thread_task;
    p->temp_file->file.thread_handler = p->thread_handler;
    ...
}
n = ngx_write_chain_to_temp_file(p->temp_file, out);
if (n == NGX_AGAIN) {              // write was posted to a thread, not completed
    p->writing = out;             // remember the chain being written
    p->thread_task = p->temp_file->file.thread_task;
    return NGX_AGAIN;
}
```

The post happens in `ngx_http_upstream_thread_handler`
(`ngx_http_upstream.c:4159`):

```c
task->event.handler = ngx_http_upstream_thread_event_handler;
ngx_thread_task_post(tp, task);   // hand the writev() to a pool worker
r->main->blocked++;
r->aio = 1;
p->aio = 1;                       // event loop free while the worker runs
ngx_add_timer(&task->event, 60000);
```

While outstanding, the producer refuses new work — `ngx_event_pipe_read_upstream`
(`ngx_event_pipe.c:119`):

```c
if (p->aio)     return NGX_AGAIN;   // an async op is running; don't touch buffers
if (p->writing) {                   // a write finished? drive it to completion
    rc = ngx_event_pipe_write_chain_to_temp_file(p);
    if (rc != NGX_OK) return rc;
}
```

and the top of `write_chain_to_temp_file` (`ngx_event_pipe.c:737`):

```c
if (p->writing) {
    if (p->aio) return NGX_AGAIN;           // still running
    out = p->writing; p->writing = NULL;    // done
    n = ngx_write_chain_to_temp_file(p->temp_file, NULL);  // finalize offsets
    goto done;                              // build p->out file bufs, recycle raw bufs
}
```

### Wake-up and resumption — `ngx_http_upstream_thread_event_handler` (`ngx_http_upstream.c:4234`)

```c
r->main->blocked--;
r->aio = 0;                 // clear async flag
...
r->write_event_handler(r);  // = ngx_http_upstream_process_downstream
ngx_http_run_posted_requests(c);
```

`ngx_http_upstream_process_downstream` (`ngx_http_upstream.c:4331`) re-enters:
`p->aio = r->aio;` (now 0), calls `ngx_event_pipe(p, 1)`, then
`ngx_http_upstream_process_request` (`ngx_http_upstream.c:4420`) — which, seeing
`p->writing && !p->aio`, calls `ngx_event_pipe` once more to drive the finished
write to completion before re-evaluating `upstream_done`/store.

### Read offload — same mechanism, in `ngx_output_chain`

`ngx_output_chain_copy_buf` (`ngx_output_chain.c:588`):

```c
if (ctx->thread_handler) {
    src->file->thread_task = ctx->thread_task;
    src->file->thread_handler = ctx->thread_handler;
    n = ngx_thread_read(src->file, dst->pos, size, src->file_pos, ctx->pool);
    if (n == NGX_AGAIN) { ctx->thread_task = src->file->thread_task; return NGX_AGAIN; }
}
```

`NGX_AGAIN` propagates up (the `ngx_output_chain` loop guards on
`if (ctx->aio) return NGX_AGAIN;`), the upstream output filter copies `r->aio`
into `p->aio`, and the pipe parks exactly as on the write side. With
`aio threads` + sendfile, `ngx_output_chain_as_is` (`ngx_output_chain.c:258`)
attaches the thread handler to the file so a blocking `sendfile` page-in is
offloaded too (the `sendfile_task` / `c->sendfile_task` special-case handles
re-entrancy).

### Why `blocked` and the HTTP/2 note matter

- `r->main->blocked++`/`--` keeps the request alive while a worker thread holds a
  pointer into its pool; the `NGX_HTTP_MAX_BLOCKED` cap bounds concurrent async
  ops per request.
- The `#if (NGX_HTTP_V2)` branches (`ngx_http_upstream.c:4178` and `:4263`)
  re-point `c` to the HTTP/2 **main connection** and nudge its write event —
  because under h2 many streams share one connection, a threaded sendfile
  completion must drive the shared connection's output. This is the one place the
  protocol layer visibly reaches into the otherwise protocol-agnostic machinery.

---

<a name="18-example-config"></a>
## 18. A working `proxy_cache_path` + `proxy_cache` config with an h2/h3 listener

Requires nginx built `--with-http_v3_module` and `--with-http_ssl_module`.

```nginx
http {
    # Shared-memory index (keys_zone) + on-disk payload tree.
    #   levels=1:2       -> /var/cache/nginx/<1 hex>/<2 hex>/<32-hex-md5>
    #   keys_zone=STATIC:10m  -> rbtree/LRU index (~80k entries per MB)
    #   max_size=10g     -> cache manager evicts (LRU) above this
    #   inactive=60m     -> drop entries not requested within 60 min
    #   use_temp_path=off-> write temp files directly under the cache dir (atomic rename)
    proxy_cache_path /var/cache/nginx/static
                     levels=1:2
                     keys_zone=STATIC:10m
                     max_size=10g
                     inactive=60m
                     use_temp_path=off;

    upstream backend {
        server 127.0.0.1:8080;
        keepalive 32;
    }

    server {
        # --- Client-facing protocols: all three terminate here ---
        listen 443 ssl;            # HTTP/1.1 and HTTP/2 over TLS
        listen 443 quic reuseport; # HTTP/3 (QUIC)
        http2 on;

        server_name example.com;

        ssl_certificate     /etc/nginx/ssl/example.com.crt;
        ssl_certificate_key /etc/nginx/ssl/example.com.key;
        ssl_protocols       TLSv1.2 TLSv1.3;   # TLS 1.3 required for HTTP/3

        # Advertise HTTP/3 to clients currently on h1/h2.
        add_header Alt-Svc 'h3=":443"; ma=86400' always;

        # Expose which cache path was taken (MISS/HIT/EXPIRED/REVALIDATED/...).
        add_header X-Cache-Status $upstream_cache_status always;

        location / {
            proxy_pass http://backend;        # nginx->backend is HTTP/1.1

            # --- Cache engine wiring (all upstream-side) ---
            proxy_cache STATIC;

            # Primary key -> MD5 -> c->main (§8).
            proxy_cache_key "$scheme$proxy_host$request_uri";

            # Fallback freshness when origin sends no Cache-Control/Expires.
            proxy_cache_valid 200 301 302 10m;
            proxy_cache_valid 404 1m;

            # §7: send If-Modified-Since/If-None-Match on EXPIRED,
            # accept 304 -> REVALIDATED (header-only refresh, body reused).
            proxy_cache_revalidate on;

            # Thundering-herd protection: one request fills a MISS, others wait (§9).
            proxy_cache_lock on;
            proxy_cache_lock_timeout 5s;

            # Only cache the Nth+ request for a key (SCARCE path).
            proxy_cache_min_uses 1;

            # Resilience: serve stale while refreshing / on backend trouble.
            proxy_cache_use_stale updating error timeout http_500 http_502 http_503 http_504;
            proxy_cache_background_update on;

            # Honor backend directives that must defeat caching.
            proxy_ignore_headers Set-Cookie;         # (optional) don't let Set-Cookie disable caching
            proxy_cache_bypass $http_pragma;         # Pragma: no-cache -> status BYPASS
            proxy_no_cache     $http_authorization;  # don't store authorized responses

            # Buffering MUST be on for caching (§15); it is on by default.
            proxy_http_version 1.1;
            proxy_set_header Connection "";          # enable upstream keepalive
        }
    }
}
```

Confirm protocol-independence at runtime: request the same URL with
`curl --http1.1`, `curl --http2`, and `curl --http3` and watch `X-Cache-Status`
go `MISS` then `HIT` across all three regardless of order.

---

<a name="19-end-to-end-summary"></a>
## 19. End-to-end journey summary

```
BACKEND (HTTP/1.x, FastCGI, …)
   │ recv_chain
   ▼
raw buffer ──shadow──► p->in (data bufs)        [proxy_buffers]
   │ input_filter decrements p->length
   │   p->length==0 ─► upstream_done   (clean complete)
   │   recv==0      ─► upstream_eof    (store only if length==-1 & match)
   │   recv<0/err   ─► upstream_error  (discard)
   ▼ (cacheable: always spill)
write_chain_to_temp_file ──(aio threads)──► p->writing/p->aio ──► thread pool writev
   │  builds p->out = {file,file_pos,file_last,in_file=1}; recycles raw buf→free_raw_bufs
   ▼ upstream_done
ngx_http_file_cache_update: temp file --atomic rename--> cache file; node->exists=1
                            (index node lives in keys_zone SLAB; found via rbtree lookup)
   ▼ serve (this request and future HITs)
p->output_filter → ngx_http_output_filter → ngx_output_chain
   │  as_is & sendfile ─► file descriptor passes through untouched
   │  needs memory     ─► ngx_thread_read window into RAM (aio threads)
   ▼
write filter → c->send_chain → sendfile(2): cache file ──► client socket
   ▼  (re-framed by the client's protocol layer)
CLIENT over HTTP/1.1, HTTP/2, or HTTP/3 — identical cached bytes
```

**Key invariants:**
- The body exists as **one complete file**; the index is **one slab node**
  reachable in O(log n); the client is served by **streaming that file**, ideally
  with zero-copy `sendfile`.
- Disk writes (spooling) and disk reads (serving through in-memory filters) are
  both **offloadable to a thread pool**, with `p->writing`/`p->aio`/`r->aio` as
  the handshake that parks and resumes the pipe without blocking the worker.
- Every protocol-specific concern lives **downstream of the cache** (framing at
  send time) or is a narrow `NGX_HTTP_V2` re-entrancy fix-up — the cache engine
  itself never knows which protocol the client used.

**On a single cold MISS the three subsystems interlock:**
1. Request misses; `exists()` inserts a node (`exists=0`), `lock()` makes it the
   updater (`node->updating=1`), others park on 500 ms timers.
2. Backend responds → body streamed to a temp file inside the cache dir.
3. `update()` renames the temp file in, sets `node->exists=1`, `node->fs_size`,
   adds to `sh->size`, clears `node->updating`.
4. Waiters wake on their next tick, re-open, and now get a HIT; `exists()` bumps
   the node to the LRU head.
5. Later, if `sh->size` exceeds `max_size` (or `keys_zone` fills), the manager's
   `forced_expire` deletes this node from the LRU tail once it's idle enough.
