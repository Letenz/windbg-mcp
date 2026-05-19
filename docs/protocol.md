# Pipe protocol

The wire protocol between the WinDbg extension DLL (`mcpext.dll`) and the
MCP host process (`windbg-mcp.exe`). Versioned so we can extend later
without breaking AI clients.

```
protocol_version = 1
pipe_name        = \\.\pipe\windbgmcp
pipe_mode        = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
```

## Framing

Each frame on the wire is:

```
+------------------+----------------------------+
|  4 bytes         |  N bytes                   |
|  uint32 LE       |  UTF-8 JSON payload        |
|  payload length  |                            |
+------------------+----------------------------+
```

- Length is the **payload byte count**, not including the 4-byte header.
- Maximum single frame: **16 MiB** (hard cap; anything larger is a protocol
  error and the receiver disconnects).
- Frames are independent: do not assume any temporal ordering between `resp`
  and `event` frames. They may interleave freely.

## Frame kinds

Every payload is a JSON object with a `frame` discriminator. Four kinds.

### `req` — request (server → ext)

```jsonc
{
  "frame": "req",
  "id":    42,                  // uint32, unique per in-flight request
  "op":    "run_cmd",           // see "Operations" below
  "args":  { /* op-specific */ }
}
```

- `id` must be unique among requests currently in flight. The server is
  responsible for picking ids; the ext does not generate them.
- The server may have multiple `req` frames in flight concurrently on a single
  connection. The ext dispatches each to a worker thread.

### `resp` — response (ext → server)

```jsonc
// success
{
  "frame": "resp",
  "id":    42,
  "ok":    true,
  "data":  { /* op-specific */ }
}

// error
{
  "frame": "resp",
  "id":    42,
  "ok":    false,
  "err": {
    "code": "timeout",          // see "Error codes" below
    "msg":  "...",              // human-readable detail
    "tip":  "...",              // AI-actionable next step
    "hr":   "0x80004005"        // optional, dbgeng HRESULT if present
  }
}
```

Exactly one `resp` frame is emitted per `req`, unless the request switched to
`chunk` streaming — see below.

### `event` — async notification (ext → server)

```jsonc
{
  "frame": "event",
  "kind":  "bugcheck",          // see docs/events.md
  "ts":    1729872345123,       // unix ms, UTC
  "data":  { /* event-specific */ }
}
```

- Events have **no `id`** and never correlate with a request.
- The ext maintains a 30-second / 64-entry ring buffer of events so the server
  can request a replay (see `docs/events.md`).

### `chunk` — streamed response body (ext → server)

```jsonc
{
  "frame":  "chunk",
  "id":     42,
  "seq":    0,                  // monotonic, starts at 0
  "eof":    false,              // true on the final chunk
  "chunk":  "..."               // partial UTF-8 text
}
```

Streaming is triggered by the request — for `run_cmd`, by `args.output_file`
being non-empty. The frame sequence is:

```
chunk(id=42, seq=0, eof=false, chunk="...")
chunk(id=42, seq=1, eof=false, chunk="...")
chunk(id=42, seq=N, eof=true,  chunk="...")    // last chunk may be empty
resp (id=42, ok=true, data={...})              // summary, no body
```

The terminating `resp` always carries metadata (byte count, exec status after)
even in streaming mode.

## Operations

The complete list of `op` values accepted by the ext.

| `op`             | args                                                              | data on success |
|------------------|-------------------------------------------------------------------|-----------------|
| `session`        | `{}`                                                              | session snapshot |
| `run_cmd`        | `{cmd: str, timeout_ms?: int, output_file?: str, preview_bytes?: int}` | command result |
| `wait_event`     | `{kinds: [str], timeout_ms: int, since_ms?: int}`                 | matched event |
| `break_in`       | `{timeout_ms?: int}`                                              | `{prior_status, current_status}` |
| `exit`           | `{}`                                                              | `{ok: true}` |

Note: `analyze_crash` is **not** an ext op. The server-side tool composes it
from multiple `run_cmd` calls.

## Operation details

### `session`

Snapshot of the dbgeng state. Cheap; intended for liveness checks.

```jsonc
// data
{
  "attached":       true,
  "target_kind":    "kernel",     // "kernel" | "user" | "dump" | "none"
  "exec_status":    "BREAK",      // dbgeng DEBUG_STATUS_* normalized to text
  "ip":             "0xfffff800`12345678",
  "bugcheck":       null,         // or {code, name, params}
  "modules_count":  324,
  "thread_id":      4,
  "ext_version":    "1.0.0"
}
```

### `run_cmd`

Raw passthrough to `IDebugControl::Execute`. The cmd string is fed verbatim,
including embedded newlines (interpreted by dbgeng as separate commands).

Args:

- `cmd` (required) — the command text, UTF-8.
- `timeout_ms` — defaults to STANDARD (30000). Hard ceiling enforced server-side.
- `output_file` — absolute path. Triggers `chunk` streaming. Server writes
  every chunk to this file as it arrives; the final `resp.data.output` carries
  only a preview, not the whole body.
- `preview_bytes` — head bytes in the preview. Default 8192. The preview always
  also includes the last 2 KiB.

Success `data`:

```jsonc
{
  "output":               "...",      // preview (head + tail), or full body if < 256 KiB
  "output_file":          "F:/...",   // echoed when output_file was set
  "bytes_total":          123456,
  "truncated_in_response": true,
  "exec_status_after":    "BREAK"
}
```

Execution flags used internally: `DEBUG_EXECUTE_NO_REPEAT | DEBUG_EXECUTE_NOT_LOGGED`
— the command does not pollute the WinDbg command history pane.

### `wait_event`

Block until an event matching any of `kinds` arrives, or `timeout_ms` elapses.

Args:

- `kinds` (required) — list of event kinds to match. See `docs/events.md`.
- `timeout_ms` (required) — no internal cap; honour what the caller asked for.
- `since_ms` — optional millisecond Unix timestamp. If set, the ext first
  scans its event ring buffer for matches with `ts >= since_ms` and returns
  the earliest match immediately. If no historical match, it then subscribes
  to future events.

Success `data` is the matched event payload:

```jsonc
{
  "kind": "bugcheck",
  "ts":   1729872345123,
  "data": { /* see docs/events.md */ }
}
```

On timeout, `resp.ok = false`, `err.code = "timeout"`.

### `break_in`

Issues `IDebugControl::SetInterrupt(DEBUG_INTERRUPT_ACTIVE)` from a fresh
`IDebugClient`, then waits on the ext's own event sink for a `break` event.
Polling is not used.

Args:

- `timeout_ms` — defaults to STANDARD (30000).

Success `data`:

```jsonc
{
  "prior_status":   "GO",
  "current_status": "BREAK",
  "already_broken": false
}
```

If the target was already broken at entry, returns immediately with
`already_broken=true`.

### `exit`

Detaches the dbgeng session and signals the WinDbg UI to close. The ext
unloads its pipe server before returning. The host should treat the
connection as closed after sending this.

## Error codes

| `err.code`         | When | `tip` hints AI to |
|--------------------|------|--------------------|
| `disconnected`     | pipe broken mid-request, or never connected | restart WinDbg + `!mcpext.start` |
| `not_attached`     | dbgeng has no target | wait for KD or open a dump |
| `target_running`   | op requires broken state but target is GO | call `wm_break_in` first |
| `timeout`          | `timeout_ms` elapsed | raise timeout or check if target hung |
| `engine_error`     | dbgeng API returned non-S_OK; `err.hr` populated | inspect hr, retry |
| `invalid_arg`      | arg failed validation server-side | fix the named arg |

The host may synthesize a 7th code, `protocol_error`, for malformed
frames received from the ext. That code never originates from the ext itself.

## Timeouts

Three named tiers shared between server and ext:

| Tier         | ms     | Default for |
|--------------|--------|-------------|
| INTERACTIVE  | 5 000  | `wm_session`, `wm_exit` |
| STANDARD     | 30 000 | `wm_run_cmd`, `wm_break_in` |
| HEAVY        | 120 000| `wm_analyze_crash` (server-side composed) |
| EVENT_WAIT   | caller-supplied | `wm_wait_event` (no internal cap) |

Any `timeout_ms` supplied in `req.args` overrides the default. The ext enforces
its own deadline independently of the server; if the ext times out first it
emits a `resp` with `err.code = "timeout"`. If the server times out first it
cancels by closing the request slot — the ext is still allowed to finish but
its `resp` is discarded.

## Escaping & encoding

- All payloads are UTF-8 JSON. The JSON encoder handles every special
  character (`"`, `\`, control bytes, multi-byte UTF-8) automatically. No
  layer below the JSON encoder does string manipulation.
- WinDbg native output is ANSI in the current console codepage (usually 1252).
  The ext converts to UTF-8 via `MultiByteToWideChar(CP_ACP, ...)` →
  `WideCharToMultiByte(CP_UTF8, ...)` before placing it in a JSON string.
- `cmd` strings are passed to `IDebugControl::Execute` byte-for-byte after
  JSON decoding. The ext does **not** quote, escape, or split. Multi-command
  scripts use literal newlines in the JSON string (`"\n"`).
- Paths in `output_file` are UTF-8 in the protocol and converted to UTF-16
  (`-W` Win32 APIs) when opening the file.

## Connection lifecycle

1. `!mcpext.start` creates the pipe and listens.
2. The host (`windbg-mcp.exe`) connects (`CreateFileW` with `FILE_FLAG_OVERLAPPED` on the pipe name).
3. Either side may send `req` / `event` frames at any time.
4. Either side may close the pipe. The ext considers a closed pipe a soft
   error and re-listens — but only one client at a time may be connected.
5. `!mcpext.stop` or the `exit` op tears down the pipe server permanently
   until the next `!mcpext.start`.

## Versioning

`protocol_version = 1` is implicit in v1. Future revisions will add a
`hello` handshake (server sends first, ext replies with its version) so peers
can negotiate. Until then, both ends assume v1.
