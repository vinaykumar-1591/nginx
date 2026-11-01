# How nginx Handles HTTP/2 End-to-End

This document explains, with links to the actual source, how nginx serves an
**HTTP/2** request from protocol negotiation to stream close. HTTP/2 multiplexes
many requests over a single **TCP** connection using binary framing, **HPACK**
header compression, and per-stream **flow control** — but the top of the stack
reuses the exact same request engine and output filter chain described in
[How nginx Handles HTTP/1.1 End-to-End](nginx-http11-e2e.md). For the UDP/QUIC
variant of this story see
[How nginx Handles HTTP/3 (QUIC) End-to-End](nginx-http3-e2e.md).

All source links point at the `master` branch of
[`vinaykumar-1591/nginx`](https://github.com/vinaykumar-1591/nginx).

---

## 1. The big picture — multiplexed streams over one TCP connection

Where HTTP/1.1 maps one request to one TCP connection, HTTP/2 carries **many
logical streams** over a single TCP connection. nginx implements this as a
**byte-stream state machine**: it never blocks, it feeds received bytes into a
chain of handler functions, each parsing one frame (or partial frame) and
returning the next handler to run.

```
TCP connection                 src/http/ngx_http_request.c (accept, ALPN / h2c)
  → HTTP/2 connection          framing · HPACK · flow control · prioritization
       src/http/v2/ngx_http_v2.c
     → HTTP/2 stream           one fake ngx_connection_t per stream
          src/http/v2/ngx_http_v2.c (ngx_http_v2_create_stream)
        → generic HTTP request engine (shared with HTTP/1.1 and HTTP/3)
             src/http/ngx_http_request.c
           → HTTP/2 output filters (HPACK-encode HEADERS, frame DATA)
                src/http/v2/ngx_http_v2_filter_module.c
```

The pivotal design idea: **each HTTP/2 stream is wrapped in its own fake
`ngx_connection_t`** whose `send_chain` writes *stream frames* rather than a
socket. That lets the existing HTTP request machinery (phase engine, filters,
proxy, upstreams) run almost unchanged on top of a stream — exactly as it runs on
a TCP socket for HTTP/1.1.

Core files:

- HTTP/2 connection, framing, multiplexing: [`src/http/v2/ngx_http_v2.c`](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c)
- HTTP/2 connection/stream structs: [`src/http/v2/ngx_http_v2.h`](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h)
- Response encoding (HEADERS/DATA output): [`src/http/v2/ngx_http_v2_filter_module.c`](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c)
- HPACK dynamic table: [`src/http/v2/ngx_http_v2_table.c`](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c)
- HPACK integer/string encoding: [`src/http/v2/ngx_http_v2_encode.c`](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_encode.c)
- Directives / module config: [`src/http/v2/ngx_http_v2_module.c`](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_module.c)

---

## 2. Connection & request lifecycle (negotiation → close)

| Stage | Function | Source |
|---|---|---|
| ALPN advertises `h2` (over TLS) | ALPN select callback | [ngx_http_ssl_module.c#L514](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/modules/ngx_http_ssl_module.c#L514) |
| h2c preface detection (cleartext) | `ngx_http_wait_request_handler` | [ngx_http_request.c#L502](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/ngx_http_request.c#L502) |
| Initialize HTTP/2 connection | `ngx_http_v2_init` | [ngx_http_v2.c#L205](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L205) |
| Event loop feeds bytes | `ngx_http_v2_read_handler` | [ngx_http_v2.c#L333](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L333) |
| Parse 9-byte frame header, dispatch | `ngx_http_v2_state_head` | [ngx_http_v2.c#L883](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L883) |
| Per-type frame dispatch table | `ngx_http_v2_frame_states[]` | [ngx_http_v2.c#L187](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L187) |
| New request stream | `ngx_http_v2_state_headers` | [ngx_http_v2.c#L1166](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1166) |
| Create stream + fake connection | `ngx_http_v2_create_stream` | [ngx_http_v2.c#L3064](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L3064) |
| HPACK-decode a header field | `ngx_http_v2_state_process_header` | [ngx_http_v2.c#L1702](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1702) |
| Reconstruct request, hand off | `ngx_http_v2_run_request` | [ngx_http_v2.c#L3947](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L3947) |
| Into the shared engine | `ngx_http_process_request` | [ngx_http_v2.c#L4097](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4097) |
| Request body from DATA frames | `ngx_http_v2_read_request_body` | [ngx_http_v2.c#L4106](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4106) |
| Response headers → HEADERS frame | `ngx_http_v2_header_filter` | [ngx_http_v2_filter_module.c#L107](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L107) |
| Response body → DATA frames | `ngx_http_v2_send_chain` | [ngx_http_v2_filter_module.c#L1063](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1063) |
| Prioritized multiplex, one writev | `ngx_http_v2_send_output_queue` | [ngx_http_v2.c#L507](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L507) |
| Close one stream | `ngx_http_v2_close_stream` | [ngx_http_v2.c#L4704](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4704) |

### One-line mental model

```
TLS ALPN "h2" / h2c preface
      → ngx_http_v2_init           (handshake: send SETTINGS + WINDOW_UPDATE)
      → ngx_http_v2_read_handler   (event loop feeds bytes)
      → ngx_http_v2_state_head     (parse 9-byte frame header, dispatch)
          ├─ HEADERS → HPACK decode → ngx_http_v2_run_request → ngx_http_process_request
          ├─ DATA    → flow control → request body
          ├─ SETTINGS / WINDOW_UPDATE / PING / RST_STREAM / GOAWAY → connection control
      ← ngx_http_v2_header_filter + ngx_http_v2_send_chain  (HPACK-encode, frame, flow-control)
      ← ngx_http_v2_send_output_queue  (prioritized multiplex, single writev)
      → ngx_http_v2_close_stream / finalize_connection
```

### Lifecycle sequence diagram

```mermaid
sequenceDiagram
    participant Cl as Client
    participant TCP as TCP accept<br/>(ngx_http_request.c)
    participant H2 as HTTP/2 connection<br/>(ngx_http_v2.c)
    participant HP as HPACK tables<br/>(ngx_http_v2_table.c)
    participant ST as HTTP/2 stream<br/>(fake ngx_connection_t)
    participant ENG as HTTP engine<br/>(ngx_http_request.c)
    participant FLT as v2 output filters<br/>(ngx_http_v2_filter_module.c)

    Cl->>TCP: TLS ClientHello (ALPN: h2) or h2c preface
    TCP->>H2: ngx_http_v2_init (send SETTINGS + conn WINDOW_UPDATE)
    Cl->>H2: HEADERS frame (HPACK block, END_HEADERS)
    H2->>HP: ngx_http_v2_state_process_header (static/dynamic lookup)
    H2->>ST: ngx_http_v2_create_stream (new fake connection)
    H2->>ENG: ngx_http_v2_run_request → ngx_http_process_request
    Cl->>H2: DATA frame(s) (request body, END_STREAM on last)
    H2->>ENG: ngx_http_v2_process_request_body → upstream/disk
    ENG->>FLT: send_header / output_filter
    FLT->>HP: HPACK-encode status + headers (static table)
    FLT->>H2: queue HEADERS + DATA frames (priority-ordered)
    H2-->>Cl: ngx_http_v2_send_output_queue → writev (encrypted if TLS)
    H2->>ST: frame handler: fin → out_closed; close_stream
```

---

## 3. Protocol negotiation — how a connection *becomes* HTTP/2

There are two entry points, both converging on `ngx_http_v2_init`:

**a) Over TLS, via ALPN.** During the TLS handshake nginx's ALPN callback
advertises `h2` ([ngx_http_ssl_module.c#L514](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/modules/ngx_http_ssl_module.c#L514)).
`NGX_HTTP_V2_ALPN_PROTO` is `"\x02h2"`
([ngx_http_v2.h#L16](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L16)).
If the client selects `h2`, after the handshake the request entry routes into
`ngx_http_v2_init`.

**b) Cleartext (h2c prior knowledge).** When `listen ... http2` / `http2 on;` is
set without TLS, nginx peeks at the first bytes; if they match the HTTP/2
connection preface (`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`) it calls
`ngx_http_v2_init` ([ngx_http_request.c#L502](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/ngx_http_request.c#L502)).

### `ngx_http_v2_init` — connection setup

[ngx_http_v2.c#L205](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L205)
allocates one `ngx_http_v2_connection_t`
([ngx_http_v2.h#L124](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L124)) —
the per-connection state holding flow-control windows, the HPACK tables, the
stream index, and the output frame queue. It:

- initializes `send_window`/`recv_window`/`init_window` and `frame_size` to
  protocol defaults;
- immediately **sends a SETTINGS frame** and a connection-level **WINDOW_UPDATE**
  to grow the receive window to the max;
- sets the first parser state to `ngx_http_v2_state_preface` and installs
  `ngx_http_v2_read_handler` / `ngx_http_v2_write_handler`;
- arms `client_header_timeout` and marks the connection reusable/idle.

### The read loop and the state machine

`ngx_http_v2_read_handler`
([ngx_http_v2.c#L333](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L333))
copies any **carried-over partial bytes** from `state.buffer` into the shared
`recv_buffer`, `recv()`s more, then runs
`p = h2c->state.handler(h2c, p, end)` until the buffer drains. Each handler
consumes what it can and **returns the next handler**
(`ngx_http_v2_handler_pt`). If a frame is cut mid-buffer, the handler calls
`ngx_http_v2_state_save` to stash the remaining bytes and resume on the next read.

After the preface, every frame starts at `ngx_http_v2_state_head`
([ngx_http_v2.c#L883](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L883)),
which parses the 9-byte frame header (length, type, flags, stream id) and
dispatches through the function-pointer table `ngx_http_v2_frame_states[]`
([ngx_http_v2.c#L187](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L187)):

| Frame | Handler |
|---|---|
| DATA | `ngx_http_v2_state_data` |
| HEADERS | `ngx_http_v2_state_headers` |
| PRIORITY | `ngx_http_v2_state_priority` |
| RST_STREAM | `ngx_http_v2_state_rst_stream` |
| SETTINGS | `ngx_http_v2_state_settings` |
| PING | `ngx_http_v2_state_ping` |
| GOAWAY | `ngx_http_v2_state_goaway` |
| WINDOW_UPDATE | `ngx_http_v2_state_window_update` |
| CONTINUATION | `ngx_http_v2_state_continuation` |

(PRIORITY_UPDATE is handled specially; a client PUSH_PROMISE is rejected — server
push was removed.)

---

## 4. Request path — HEADERS → HPACK → nginx request

- `ngx_http_v2_state_headers`
  ([ngx_http_v2.c#L1166](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1166))
  validates the new stream id, enforces `http2_max_concurrent_streams`, handles
  padding/priority, and creates the stream + its request object via
  `ngx_http_v2_create_stream`
  ([ngx_http_v2.c#L3064](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L3064)).
  Each stream gets a *fake* `ngx_connection_t` so the rest of nginx works
  unchanged.
- `ngx_http_v2_state_header_block` → `ngx_http_v2_state_process_header`
  ([ngx_http_v2.c#L1702](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1702))
  decode the **HPACK**-compressed header fields (static + dynamic table in
  [ngx_http_v2_table.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c),
  optional Huffman). Pseudo-headers (`:method`, `:path`, `:scheme`,
  `:authority`) and regular headers are stored into `headers_in`.
- When headers end, `ngx_http_v2_run_request`
  ([ngx_http_v2.c#L3947](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L3947))
  reconstructs a synthetic HTTP/1-style request line, reassembles split `cookie`
  headers, rejects connection-specific headers forbidden in h2 (`Connection`,
  `Keep-Alive`, `Transfer-Encoding`, `Upgrade`, bad `TE`), validates Host vs
  `:authority`, then calls `ngx_http_process_request`
  ([ngx_http_v2.c#L4097](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4097)).
  From here the request flows through the *same* rewrite/location/content phases
  as HTTP/1.x.

---

## 5. Deep dive — HPACK dynamic-table mechanics

HPACK (RFC 7541) compresses headers using two tables that index previously-seen
`name:value` pairs; a header can then be sent as a single integer. State lives in
`ngx_http_v2_hpack_t`
([ngx_http_v2.h#L112](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L112)).

### Two tables, one index space

A client references an entry by a single index, resolved in
`ngx_http_v2_get_indexed_header`
([ngx_http_v2_table.c#L104](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c#L104)):

```
index 1 .. 61         → static table (61 fixed, well-known pairs)
index 62 .. 62+N-1    → dynamic table, most-recent first
```

The **static table**
([ngx_http_v2_table.c#L19](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c#L19))
is a compile-time constant of 61 entries (`:method: GET`, `:status: 200`,
`accept-encoding: gzip, deflate`, …). A static hit is a direct array copy — no
allocation.

### The dynamic table is a double ring buffer

Two ring structures work together
([ngx_http_v2.h#L112](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L112)):

1. **`entries[]`** — a ring of `ngx_http_v2_header_t *` pointers (metadata).
   Sized `allocated` (starts at 64, grows by 64).
2. **`storage`** — a single fixed `NGX_HTTP_V2_TABLE_SIZE` (4096-byte) byte
   buffer written as a **circular buffer** via `pos`.

Rather than head/tail pointers, nginx uses monotonic counters and modular
arithmetic: `added` (ever inserted), `deleted` (evicted), `reused` (reclaimed
pointer slots). Live count = `added - deleted`. To fetch dynamic index `i`
([ngx_http_v2_table.c#L131](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c#L131)):

```c
index = (h2c->hpack.added - index - 1) % h2c->hpack.allocated;
entry = h2c->hpack.entries[index];
```

Because `storage` is circular, a name/value can **wrap** the end, so reads and
writes split into two `memcpy`s.

### Insertion and eviction

`ngx_http_v2_add_header`
([ngx_http_v2_table.c#L189](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c#L189))
lazily allocates `entries` and the 4096-byte `storage`, then calls
`ngx_http_v2_table_account`
([ngx_http_v2_table.c#L303](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c#L303))
to enforce the size budget — where each entry costs `name.len + value.len + 32`:

```c
size += 32;
if (size <= free) { free -= size; return NGX_OK; }     // fits
if (size > hpack.size) {                                // too big for whole table
    deleted = added;                                    //   → evict everything
    free = size; return NGX_DECLINED;                   //   → entry not indexed
}
do {                                                    // evict oldest until it fits
    entry = entries[deleted++ % allocated];
    free += 32 + entry->name.len + entry->value.len;
} while (size > free);
```

Eviction is just advancing `deleted` (FIFO, oldest-out). If `table_account`
returns `NGX_DECLINED`, the header is still delivered, just not indexed. The
entry-pointer ring grows by unrolling the circular order into a linear one and
resetting the counters; storage never grows (fixed 4096). The client can also
resize the table via `ngx_http_v2_table_size`
([ngx_http_v2_table.c#L336](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c#L336)).
Index 0, out-of-bound indices, and oversized size updates are rejected.

---

## 6. Deep dive — flow-control + prioritization scheduler

### Windows: two levels, two directions

Every window is a credit counter:

| Window | Direction | Init | Meaning |
|---|---|---|---|
| `h2c->send_window` | out | 65535 | bytes nginx may send across the whole connection |
| `stream->send_window` | out | `h2c->init_window` ([ngx_http_v2.c#L3190](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L3190)) | bytes nginx may send on this stream |
| `h2c->recv_window` | in | `MAX_WINDOW` (2³¹−1) | bytes client may send on the connection |
| `stream->recv_window` | in | `preread_size` ([ngx_http_v2.c#L3191](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L3191)) | bytes client may send on this stream |

`stream->send_window` is **signed** because a `SETTINGS_INITIAL_WINDOW_SIZE`
change can retroactively push it negative
([ngx_http_v2.h#L188](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L188));
the delta is applied to every existing stream in
`ngx_http_v2_state_settings_params`
([ngx_http_v2.c#L2255](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L2255)).

### Send side — `ngx_http_v2_flow_control`

Before cutting DATA frames, `ngx_http_v2_send_chain` consults
`ngx_http_v2_flow_control`
([ngx_http_v2_filter_module.c#L1408](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1408)):

```c
if (stream->send_window <= 0) { stream->exhausted = 1; return NGX_DECLINED; }
if (h2c->send_window == 0)    { ngx_http_v2_waiting_queue(...); return NGX_DECLINED; }
return NGX_OK;
```

- **Stream exhausted** → parks until a *stream* WINDOW_UPDATE arrives.
- **Connection window zero** → stream is pushed onto `h2c->waiting`, a
  **priority-ordered** queue.

When data is sent, both windows are debited together
([ngx_http_v2_filter_module.c#L1241](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1241)):

```c
h2c->send_window    -= frame_size;
stream->send_window -= frame_size;
stream->queued++;
```

### Re-activation — `ngx_http_v2_state_window_update`

Credit returns via WINDOW_UPDATE frames
([ngx_http_v2.c#L2476](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L2476)):

- **Stream-level** (`sid != 0`): `stream->send_window += window`; if the stream
  was `exhausted`, clear it and re-fire the write handler.
- **Connection-level** (`sid == 0`): `h2c->send_window += window`, then **drain
  the `waiting` queue in priority order**, waking each parked stream until the
  connection window hits zero again.

Overflowing a window past `MAX_WINDOW` is a flow-control error.

### Receive side

Incoming DATA debits `h2c->recv_window` then `stream->recv_window`
([ngx_http_v2.c#L980](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L980),
[#L1023](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1023)).
The connection window is topped back up to `MAX_WINDOW` whenever it drops below a
quarter, so nginx basically never flow-controls the connection inbound. The
*stream* receive window starts small (`preread_size`) and is only expanded on
demand once a handler actually asks to read the body (§7) — nginx's backpressure
lever.

### Prioritization — the ordered output queue

All outbound frames live on one intrusive list `h2c->last_out`; insertion
position encodes priority. Three insert helpers
([ngx_http_v2.h#L264](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L264)):

- **`ngx_http_v2_queue_frame`** — inserts before the first frame the new frame
  *precedes* (per `ngx_http_v2_stream_precedes`
  [ngx_http_v2.h#L244](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h#L244)),
  stopping early at any `blocked` frame or control frame.
- **`ngx_http_v2_queue_blocked_frame`** — after all control/blocked frames but
  before stream data.
- **`ngx_http_v2_queue_ordered_frame`** — straight to the front.

The precedence rule implements **RFC 9218 Extensible Priorities**: lower
`urgency` wins; else non-`incremental` (a complete resource) wins; else lower
stream id (FIFO). The same comparator orders both the output queue and the
`waiting` queue.

### Draining — `ngx_http_v2_send_output_queue`

[ngx_http_v2.c#L507](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L507)
flattens the priority-ordered list into one chain and issues a **single
`writev`** for the whole connection. It runs each fully-sent frame's `handler`
(which decrements `stream->queued`, recycles the frame, flips `out_closed`), and
leaves partially-sent frames (`blocked=1`) at the head for next time. The
connection write event re-enters this via `ngx_http_v2_write_handler`
([ngx_http_v2.c#L457](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L457)).
**Anti-flood:** DATA frame allocation is capped at 10000 outstanding frames per
connection
([ngx_http_v2_filter_module.c#L1344](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1344)).

---

## 7. Deep dive — request bodies streaming through DATA frames

The challenge: the response pipeline may not have asked for the body yet when
DATA frames land. nginx uses a **preread buffer** + **stream receive-window**
backpressure.

### Phase 1 — reception

`ngx_http_v2_state_data`
([ngx_http_v2.c#L923](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L923))
validates the frame and debits windows, then hands off to
`ngx_http_v2_state_read_data`
([ngx_http_v2.c#L1063](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1063)),
which decides where the bytes go:

```
if stream->skip_data            → discard   (body not wanted / request failed)
if reading_body && buffering    → discard   (will be re-driven later)
if no content-length && !chunked→ discard
if r->request_body exists       → ngx_http_v2_process_request_body(...)   // consumer ready
else (size > 0)                 → copy into stream->preread buffer        // consumer NOT ready
```

### Phase 2 — the preread buffer (consumer not ready)

Body that arrives before a handler requested it is stashed in a per-stream
`preread` buffer sized `http2_body_preread_size`
([ngx_http_v2.c#L1121](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1121)).
Because `stream->recv_window` was initialized to exactly `preread_size`, the
client can only ever send that many unsolicited body bytes before being
flow-controlled — the overflow guarantee behind the `ALERT` check at
[ngx_http_v2.c#L1142](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1142).

### Phase 3 — consumer asks — `ngx_http_v2_read_request_body`

[ngx_http_v2.c#L4106](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4106):

1. allocates `rb->buf`;
2. **drains preread bytes** immediately into the pipeline;
3. **opens the receive window** — the backpressure release:
   - *Buffered (default)*: `stream->no_flow_control = 1` and the window grows to
     `MAX_WINDOW`
     ([ngx_http_v2.c#L4213](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4213)) —
     accept the whole body as fast as it arrives (auto-refill at the ¼ mark).
   - *Unbuffered / `filter_need_buffering`*: window grows only by
     `len - preread_size`, keeping real flow control so a slow upstream throttles
     the client.
   It then emits a WINDOW_UPDATE and flushes the output queue.
4. installs `ngx_http_v2_read_client_request_body_handler`, arms
   `client_body_timeout`, returns `NGX_AGAIN` (unless already `in_closed`).

### Phase 4 — feeding — `ngx_http_v2_process_request_body`

[ngx_http_v2.c#L4252](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4252)
copies DATA bytes into `rb->buf`; when full it runs
`ngx_http_v2_filter_request_body` (filter chain → disk / upstream) and recycles
the buffer. The `END_STREAM` flag sets `rb->rest = 0` to mark completion.

---

## 8. Response egress & trailers — the `out_closed`/`END_STREAM` close path

HTTP/2 streams are **half-closed state machines**; each direction closes
independently and `END_STREAM` signals "this side is done":

- **`in_closed`** — the *client* finished sending — set from `END_STREAM` on an
  inbound HEADERS ([ngx_http_v2.c#L1318](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1318)),
  DATA ([ngx_http_v2.c#L1108](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1108)),
  or RST_STREAM ([ngx_http_v2.c#L2178](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L2178)).
- **`out_closed`** — *nginx* finished sending — set **after** the last
  `END_STREAM`-bearing frame actually leaves the wire.

### Response headers and the trailer deferral

`ngx_http_v2_header_filter`
([ngx_http_v2_filter_module.c#L107](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L107))
HPACK-encodes status + headers into a HEADERS frame. The `END_STREAM` flag goes
on the HEADERS frame **only** if there is no body *and* no trailers
([ngx_http_v2_filter_module.c#L625](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L625)):

```c
fin = r->header_only
      || (r->headers_out.content_length_n == 0 && !r->expect_trailers);
```

In `ngx_http_v2_send_chain`, when the final body buffer (`last_buf`) is reached,
nginx builds a trailers frame
([ngx_http_v2_filter_module.c#L1221](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1221)):

```c
if (cl->buf->last_buf) {
    trailers = ngx_http_v2_create_trailers_frame(r);
    if (trailers != NGX_HTTP_V2_NO_TRAILERS) {
        cl->buf->last_buf = 0;   // move the "fin" off the DATA frame
    }
}
```

`ngx_http_v2_create_trailers_frame`
([ngx_http_v2_filter_module.c#L949](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L949))
returns the sentinel `NGX_HTTP_V2_NO_TRAILERS` when there are none, else a HEADERS
frame built with `fin = 1`. So the `END_STREAM` bit lands on exactly one frame:

| Response shape | Frame carrying `END_STREAM` |
|---|---|
| empty body, no trailers | the response **HEADERS** frame |
| body, no trailers | the **last DATA** frame |
| body + trailers | the **trailers HEADERS** frame |

### `fin` → `out_closed` only *after* the frame is sent

Both frame handlers funnel through `ngx_http_v2_handle_frame`
([ngx_http_v2_filter_module.c#L1658](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1658)):

```c
if (frame->fin) { stream->out_closed = 1; }
...
stream->queued--;
```

### `ngx_http_v2_close_stream` — the decision

([ngx_http_v2.c#L4704](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4704)):

```c
if (stream->queued) {                 // (1) frames still unsent
    fc->error = 1;
    fc->write->handler = ngx_http_v2_retry_close_stream_handler;
    fc->read->handler  = ngx_http_v2_retry_close_stream_handler;
    return;                           //     defer close until queue drains
}
if (!stream->rst_sent && !h2c->connection->error) {
    if (!stream->out_closed) {        // (2) response never finished → abort
        ngx_http_v2_send_rst_stream(h2c, node->id,
            fc->timedout ? NGX_HTTP_V2_PROTOCOL_ERROR
                         : NGX_HTTP_V2_INTERNAL_ERROR);
    } else if (!stream->in_closed) {  // (3) response done, client still sending
        ngx_http_v2_send_rst_stream(h2c, node->id, NGX_HTTP_V2_NO_ERROR);
    }
}                                     // (4) out_closed && in_closed → clean, no RST
```

### The `queued`/retry-handler loop

When trailers (or a final DATA frame) are still queued, close is **deferred** and
re-driven once the bytes flush:

```
close_stream (queued>0) ──► park: fc->error=1, retry handler on read+write, return
        ▲                                   │
        │                                   ▼
retry_close_stream_handler ◄── posted   real socket writable
   (queued now 0 → clean close)             │
        ▲                                   ▼
        │                      write_handler → send_output_queue → writev
   handle_stream posts fc->write ◄── frame handler: fin→out_closed=1, queued--
```

The `fc->error = 1` set in Step 0 makes `ngx_http_v2_handle_stream`
([ngx_http_v2_filter_module.c#L1669](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c#L1669))
bypass its "don't wake an exhausted/delayed stream" guards and always post the
fake write event, whose handler is now `ngx_http_v2_retry_close_stream_handler`
([ngx_http_v2.c#L4840](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4840)).
Each pass sends whatever the socket accepts; the loop self-throttles to socket
capacity and only completes the close once the `END_STREAM`-bearing frame is
fully on the wire. (The sibling `ngx_http_v2_close_stream_handler`
[ngx_http_v2.c#L4815](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4815)
is the *timeout* variant.)

---

## 9. Inbound request trailers are rejected (empirically verified)

Trailer support is **one-directional**:

| Direction | Trailers? | Mechanism |
|---|---|---|
| **Response → client** (outbound) | ✅ Supported | `ngx_http_v2_create_trailers_frame` emits a trailing HEADERS frame; `END_STREAM` moves off the last DATA frame onto it (§8) |
| **Request ← client** (inbound) | ❌ Not supported | A second HEADERS block on an open stream fails the `sid <= last_sid` check → `PROTOCOL_ERROR` GOAWAY |

Every inbound HEADERS frame goes through `ngx_http_v2_state_headers`, which
**unconditionally** treats it as a *new* request and enforces strictly
increasing, odd stream IDs
([ngx_http_v2.c#L1243](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1243)):

```c
if (h2c->state.sid % 2 == 0 || h2c->state.sid <= h2c->last_sid) {
    ngx_log_error(... "client sent HEADERS frame with incorrect identifier "
                      "%ui, the last was %ui", h2c->state.sid, h2c->last_sid);
    return ngx_http_v2_connection_error(h2c, NGX_HTTP_V2_PROTOCOL_ERROR);
}
```

Trailers arrive on the **same** stream, so `sid == last_sid`, the condition is
true, and nginx replies with a `PROTOCOL_ERROR` GOAWAY that tears down the whole
connection. There is no branch that looks up an existing `node->stream` for a
HEADERS frame. A client must therefore terminate the body with `END_STREAM` on a
**DATA** frame (setting `in_closed`), not a trailing HEADERS block.

### Empirical verification

Built from this repo with `--with-http_v2_module --with-http_ssl_module`
(nginx/1.31.8), served over h2c on `127.0.0.1:8080`, and driven with a Python
`h2` client.

**Test** — `HEADERS` (no END_STREAM) → `DATA` (no END_STREAM) → **trailing
`HEADERS` with END_STREAM**:

```
ConnectionTerminated error_code:ErrorCodes.PROTOCOL_ERROR, last_stream_id:1
```

nginx error log (exact source message from
[ngx_http_v2.c#L1244](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L1244)):

```
client sent HEADERS frame with incorrect identifier 1, the last was 1
while processing HTTP/2 connection, client: 127.0.0.1, server: 127.0.0.1:8080
```

**Control** — identical request but ending the body with `END_STREAM` on the DATA
frame (no trailers): **no** `PROTOCOL_ERROR` GOAWAY, normal `StreamEnded`. The
only difference between the two runs is the trailing HEADERS block, isolating it
as the cause.

---

## 10. Connection lifecycle & shutdown

- **SETTINGS** (`ngx_http_v2_state_settings_params`
  [ngx_http_v2.c#L2255](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L2255))
  adjusts window size, header-table size, max frame size, etc., and is ACKed.
- **WINDOW_UPDATE**
  ([ngx_http_v2.c#L2476](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L2476))
  refills send windows and wakes waiting streams.
- **PING** is echoed with ACK; **RST_STREAM** aborts a single stream; **GOAWAY**
  begins graceful connection shutdown.
- Streams end via `ngx_http_v2_close_stream`
  ([ngx_http_v2.c#L4704](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c#L4704));
  the HTTP core reaches it from
  [ngx_http_request.c#L3970](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/ngx_http_request.c#L3970).
  Nodes are kept in a reuse pool, and the whole connection is torn down by
  `ngx_http_v2_finalize_connection`.

---

## 11. Source reference index

| Area | File |
|---|---|
| HTTP/2 connection, framing, multiplexing | [ngx_http_v2.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.c) |
| HTTP/2 connection/stream/HPACK structs | [ngx_http_v2.h](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2.h) |
| Response encoding (HEADERS/DATA, trailers) | [ngx_http_v2_filter_module.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_filter_module.c) |
| HPACK dynamic table | [ngx_http_v2_table.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_table.c) |
| HPACK integer/string encoding | [ngx_http_v2_encode.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_encode.c) |
| HTTP/2 module config / directives | [ngx_http_v2_module.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/v2/ngx_http_v2_module.c) |
| ALPN `h2` negotiation | [ngx_http_ssl_module.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/modules/ngx_http_ssl_module.c) |
| h2c preface detection / shared engine | [ngx_http_request.c](https://github.com/vinaykumar-1591/nginx/blob/master/src/http/ngx_http_request.c) |

> Note: line numbers refer to the `master` branch of
> [`vinaykumar-1591/nginx`](https://github.com/vinaykumar-1591/nginx) at the time of
> writing and may drift as the code changes.
