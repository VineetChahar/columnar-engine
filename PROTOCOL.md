# Wire protocol

`columnar_server` speaks a small, length-prefixed binary protocol over TCP.
This document is written so a third party could implement a client (or a
server) without reading `wire_protocol.cpp` -- every byte is accounted for.

All multi-byte integers are little-endian (the engine's on-disk format
makes the same choice for the same reason -- see `DESIGN.md` section 6 --
and every platform this project targets is little-endian natively).

## Frame

Every message, in either direction, is one frame:

| field         | type       | notes                                     |
|---------------|------------|--------------------------------------------|
| `frame_length`| `uint32`   | byte count of everything after this field   |
| `version`     | `uint8`    | protocol version, currently always `1`      |
| `message_type`| `uint8`    | see below                                   |
| `payload`     | `frame_length - 2` bytes | message-type-specific, below  |

A client sends exactly one `QueryRequest` frame, then reads exactly one
response frame (`QueryResultOk` or `QueryError`), then may send another
request on the same connection. The server never sends a frame the client
didn't ask for.

## Message types

| value | name           | direction       |
|-------|----------------|-----------------|
| `1`   | `QueryRequest` | client -> server |
| `2`   | `QueryResultOk`| server -> client |
| `3`   | `QueryError`   | server -> client |

### `QueryRequest` payload

The raw UTF-8 bytes of one SQL statement. No length prefix beyond the
frame's own `frame_length` -- the whole payload *is* the query text.

### `QueryError` payload

The raw UTF-8 bytes of a human-readable error message (parse/bind errors
include a `line:col:` prefix -- see `QueryError` in `lexer.hpp`).

### `QueryResultOk` payload

The entire result set, materialized (every row, not streamed in pieces --
see "Scoped simplifications" below), column-major:

```
uint32   column_count
repeated column_count times:
    uint32   name_len
    byte[name_len]   name (UTF-8)
    uint8    type_tag        -- 0=INT64, 1=DOUBLE, 2=BOOL, 3=TEXT
uint64   row_count
repeated column_count times (one column, all its rows, in order):
    repeated row_count times:
        uint8    validity    -- 0 = NULL (no value bytes follow), 1 = valid
        <value>              -- only present if validity == 1, per type_tag:
                                 INT64:  int64  (8 bytes)
                                 DOUBLE: float64 (8 bytes, IEEE 754)
                                 BOOL:   uint8   (1 byte, 0 or 1)
                                 TEXT:   uint32 len, then byte[len] (UTF-8)
```

`type_tag` values are `columnar::ExecType`'s underlying values: every column
in a result set is one of these four runtime types regardless of the
on-disk storage type it came from (`int32` columns are promoted to `int64`
at scan time -- see `exec_batch.hpp`).

## Scoped simplifications (disclosed, not accidental)

- **No streaming.** A `QueryResultOk` carries the whole result set in one
  frame. A truly large result set would want to stream batches as separate
  frames (the frame format already supports this -- a future
  `QueryResultBatch` / `QueryResultEnd` pair of message types would do it
  without changing anything above); not implemented here because the
  demo/benchmark workloads (Phase 6) don't produce results large enough to
  make single-frame buffering the bottleneck.
- **One query in flight per connection.** The client must fully read a
  response before sending the next request; there's no request pipelining
  or multiplexing. A connection pool (many short-lived connections, or one
  connection per concurrent client) is how a real client should get
  concurrency, and is exactly what the server's thread-per-connection
  (via a fixed thread pool) model expects.
- **No authentication, no TLS.** Out of scope for an embedded analytics
  engine's demo server -- see the zero-credentials/local-only posture this
  whole project already follows.
