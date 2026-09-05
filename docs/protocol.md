# Pipe protocol

The wire protocol between the WinDbg extension DLL (`mcpext.dll`) and the
MCP host process (`windbg-mcp.exe`). Versioned so we can extend later
without breaking AI clients.

```
protocol_version = 2
default_endpoint = \\.\pipe\windbgmcp
pipe_mode        = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
```

## Endpoint selection

Endpoint selection happens before a connection is established and does not
change the v2 wire format.

- Extension: `!mcpext.start [pipe-name|\\.\pipe\pipe-name]`
- Host CLI: `windbg-mcp.exe --pipe <pipe-name|\\.\pipe\pipe-name>`
- Host environment fallback: `WINDBGMCP_PIPE`
- Default: `\\.\pipe\windbgmcp`

Host precedence is CLI, environment, default. Both short names and full local
endpoints are canonicalized to `\\.\pipe\<name>`. Names may contain ASCII
letters, digits, `.`, `_`, and `-`, and are limited to 240 characters. Remote
UNC paths and additional path separators are rejected. The host and extension
must select the same endpoint.

Each extension endpoint has one pipe instance and accepts one host connection.
Multiple WinDbg/host pairs may run concurrently when every pair has a unique
endpoint.

The server pipe handle remains open across ordinary client disconnects. The
extension calls `DisconnectNamedPipe` and listens again on that same instance;
it does not close/recreate the handle and expose an endpoint-ownership gap.

Endpoint names are routing addresses, not bridge identities. Each successful
`!mcpext.start` creates a fresh 128-bit `bridge_instance_id`. The extension
sends it in the first frame and the host pins the first observed value for its
process lifetime. A reconnect to the same endpoint is accepted only when the
instance id is unchanged; binding a deliberately restarted extension requires
restarting `windbg-mcp.exe`. This prevents endpoint reuse from silently moving
an MCP session to another WinDbg.

## Connection generations

Each successful `ConnectNamedPipe` receives a non-zero, monotonically
increasing generation local to the extension process. Generation is transport
metadata and is not added to every wire frame:

- the reader tags every decoded request with its connection generation;
- queued work is dropped if that generation is no longer current;
- every correlated `resp` and `chunk` send rechecks the generation while
  holding the write lock;
- a late close for an old generation cannot clear a newer connection.

An already executing dbgeng call cannot be preempted safely, but all of its
post-disconnect response/chunk output is fenced from the replacement client.

## Extension lifecycle and unload barrier

Lifecycle is serialized as `Stopped → Starting → Running → Stopping → Stopped`.
Startup creates the teardown storage and dormant executor before publishing
`Running`; a request gate rejects frames received before that commit. Stop then
moves all session resources and publishes `Stopping` under the lifecycle mutex,
arms the already-created executor, and finally publishes `Stopped`. Teardown
never rolls back to `Running` after irreversible transport changes.

The DLL exports `DebugExtensionCanUnload`. It returns `S_OK` only when state is
`Stopped` and activity counters show no Router worker, callback actor, event
publisher, or teardown. Otherwise it returns `S_FALSE`, so concurrent
unload cannot free code that still has an active stack. The `run_cmd` denylist
is an operator-facing early error, not the unload safety mechanism.

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

Every payload is a JSON object with a `frame` discriminator. Six kinds.

### `hello` — bridge identity (ext → server, first frame)

```jsonc
{
  "frame": "hello",
  "protocol_version": 2,
  "bridge_instance_id": "96f0...e441",
  "pipe_endpoint": "\\\\.\\pipe\\windbgmcp-project-a"
}
```

The host must validate and pin this identity before writing its first `req`.
Events and responses are not allowed ahead of the greeting.

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
- The server may assign independent ids to queued requests. The ext executes
  requests FIFO on one stable dbgeng worker thread. This is intentional:
  `IDebugClient` is thread-affine, so a worker pool must not reuse one client
  across arbitrary threads. Async `event` frames may still interleave with a
  response. In v2 the lane also contains `wait_event`, so a long event wait
  delays later requests on that endpoint; separate endpoints remain
  independent. The FIFO is capped at 256 requests; overflow receives a
  structured `engine_error` instead of consuming unbounded extension memory.

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

A terminal response additionally contains `"terminal": true`. The host must
write the transport ACK below before exposing that response to its caller.

### `ack` — terminal response consumption (server → ext)

```jsonc
{"frame":"ack", "terminal":true, "id":42}
```

This control frame is consumed by the pipe layer and never enters the dbgeng
request lane. It replaces unbounded `FlushFileBuffers` synchronization.

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
| `detach`         | `{}`                                                              | explicit target-only detach result |
| `shutdown`       | `{}`                                                              | terminal bridge-only shutdown result |
| `exit`           | `{}`                                                              | deprecated alias for `detach` |

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
  "pipe_endpoint":  "\\\\.\\pipe\\windbgmcp-project-a",
  "connection_generation": 7,
  "event_queue_dropped": 0,
  "engine_lane":    "serial",
  "ext_version":    "2.0.0"
}
```

When a live target is running, the liveness response skips KD-backed register,
thread, module, and bugcheck queries. The corresponding numeric fields are
zero and `bugcheck` is `null` until the target is broken in.

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

### `detach`

Dispatches by debuggee class and leaves the extension pipe, MCP host, and
WinDbg UI running. User-mode targets use `DetachProcesses`; live kernel targets
use `EndSession(DEBUG_END_ACTIVE_DETACH)` so the kernel connection is actually
disconnected without terminating the target. Dump/image sessions use passive
session cleanup and report `target_detached=false`. Success data for a live
kernel target is:

```jsonc
{
  "ok": true,
  "semantic": "detach_target_only",
  "target_detached": true,
  "target_kind": "kernel",
  "detach_api": "IDebugClient::EndSession(DEBUG_END_ACTIVE_DETACH)",
  "bridge_state": "running",
  "connection_generation": 7
}
```

The HRESULT is checked. Failure is returned as `engine_error` instead of a
best-effort success.

### `shutdown`

Stops the extension bridge without calling dbgeng or changing the target.
Success data is:

```jsonc
{
  "ok": true,
  "semantic": "bridge_shutdown_only",
  "target_detached": false,
  "bridge_state": "stopping_after_response",
  "host_state": "running",
  "connection_generation": 7
}
```

This is a terminal response. The Router asks the pipe to write the complete
framed response with a hard deadline while the request generation is still
current. The host reader ACKs the frame before fulfilling the MCP request.
Only after that bounded ACK succeeds does the pipe atomically retire the
generation, stop accepting connections, and arm the prepared teardown executor.
If the generation became stale or the response write failed, teardown is
suppressed. Requests queued behind `shutdown` receive no response from that
extension instance and the host resolves them as `disconnected`.

The stdio MCP host does not exit, but it remains pinned to both the configured
endpoint and the original `bridge_instance_id`. After a deliberate
`!mcpext.stop`/restart, restart the host to bind the new instance. It will not
silently reconnect to another WinDbg that reused the endpoint name.

### `exit` (deprecated)

Compatibility alias for `detach`. Its success data also contains
`"deprecated": true` and `"replacement": "wm_detach"`. It never means
bridge shutdown.

## Error codes

| `err.code`         | When | `tip` hints AI to |
|--------------------|------|--------------------|
| `disconnected`     | selected endpoint is absent, mismatched, busy, or broke mid-request | start the extension and host with the same endpoint |
| `not_attached`     | dbgeng has no target | wait for KD or open a dump |
| `target_running`   | op requires broken state but target is GO | call `wm_break_in` first |
| `timeout`          | `timeout_ms` elapsed | raise timeout or check if target hung |
| `engine_error`     | dbgeng API returned non-S_OK; `err.hr` populated | inspect hr, retry |
| `invalid_arg`      | arg failed validation server-side | fix the named arg |

`run_cmd` also returns `invalid_arg` for lifecycle-changing commands such as
`!mcpext.*`, `.reboot`, `.unload`, and debugger quit. Invoke
`!mcpext.stop` in WinDbg first, then run the teardown command there.

The host may synthesize a 7th code, `protocol_error`, for malformed
frames received from the ext. That code never originates from the ext itself.

## Timeouts

Three named tiers shared between server and ext:

| Tier         | ms     | Default for |
|--------------|--------|-------------|
| INTERACTIVE  | 5 000  | `wm_session`, `wm_detach`, `wm_shutdown`, `wm_exit` |
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

1. `!mcpext.start [endpoint]` validates, canonicalizes, creates, and claims the
   selected pipe. If another extension already owns it, startup fails instead
   of retrying silently.
2. The host (`windbg-mcp.exe [--pipe endpoint]`) connects with `CreateFileW`
   and `FILE_FLAG_OVERLAPPED` on that same endpoint.
3. The extension sends `hello` first. The host pins its bridge instance id;
   only then may it send `req`. Event delivery uses a 256-frame bounded queue
   off the dbgeng callback thread; overflow drops newest wire events while the
   history ring and in-process waiters still receive them.
4. Either side may close the pipe. The ext fences that generation, disconnects
   the client, and re-listens on the same retained server handle. Only one
   client at a time may be connected.
5. `!mcpext.stop` transitions to `Stopping`, hands the resources to a reaper,
   and returns immediately. Outside the WinDbg extension command, the reaper
   fences/closes the pipe, cancels event waits, uninstalls the callback client
   on its owner thread, joins the Router worker, and publishes `Stopped`.
   `DebugExtensionCanUnload` remains `S_FALSE` until that work is complete.
6. `shutdown` takes the same asynchronous teardown path, but first completes a
   bounded terminal response/ACK exchange and atomically closes only the
   requesting generation. The host keeps stdio open but refuses a different
   bridge identity on that endpoint.
7. `detach` and deprecated `exit` affect the target only; they do not change
   bridge lifecycle state.

## Versioning

`protocol_version = 2` is explicit in the server-first `hello`. Version 2 adds
bridge-instance pinning and terminal ACKs and is intentionally incompatible
with the former un-greeted v1 connection. The current
host requires that greeting and refuses unversioned bridge connections.
