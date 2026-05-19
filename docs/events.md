# Events

Events are asynchronous notifications pushed by the ext over the pipe whenever
the dbgeng raises a callback we subscribed to. They are the cure for "AI can't
tell that a BSOD happened" — the AI waits on `wm_wait_event` and gets a frame
as soon as the kernel hits `KeBugCheckEx`.

## Subscription source

The ext implements `IDebugEventCallbacks` (registered on its primary
`IDebugClient`) and subscribes to:

```
DEBUG_EVENT_BREAKPOINT
DEBUG_EVENT_EXCEPTION
DEBUG_EVENT_LOAD_MODULE
DEBUG_EVENT_UNLOAD_MODULE
DEBUG_EVENT_SESSION_STATUS
DEBUG_EVENT_CHANGE_ENGINE_STATE        // for DEBUG_CES_EXECUTION_STATUS
```

All callbacks return `DEBUG_STATUS_NO_CHANGE`. They never set execution
status, never wait, never block. Each callback marshals the relevant fields
into a JSON event and hands it off to the publisher thread, which writes the
frame on the pipe and appends it to the in-process history ring.

## History replay

The ext keeps a ring buffer of recent events:

```
capacity      = 64 entries
max age       = 30 seconds
eviction      = oldest-first when full or aged out
```

`wm_wait_event(since_ms=T)` first scans the ring for events with
`ts >= T` and returns the earliest match immediately. This lets the AI
"look back" a few seconds — useful when a BSOD fires while the AI is mid-call
and would otherwise have to race to subscribe before the next event.

If `since_ms` is omitted, only future events match.

## Event kinds

Every event has the envelope:

```jsonc
{
  "frame": "event",
  "kind":  "<kind>",
  "ts":    1729872345123,
  "data":  { /* per-kind */ }
}
```

### `bugcheck` — BSOD detected

Triggered by `Exception` callback when the exception record matches a
bugcheck pattern (`ExceptionCode == STATUS_BREAKPOINT && first param ==
BugCheckCode`), or when `ChangeEngineState` reports a target stop with
non-zero `DEBUG_VALUE_BugCheckCode`.

**Payload is intentionally light.** The ext does **not** run `!analyze -v`
automatically — that's the AI's call via `wm_analyze_crash` if it wants the
full report.

```jsonc
{
  "code":   "0x000000D1",
  "name":   "DRIVER_IRQL_NOT_LESS_OR_EQUAL",
  "params": [
    "0xfffff80012345000",
    "0x0000000000000002",
    "0x0000000000000001",
    "0xfffff80012345678"
  ],
  "ip":     "0xfffff80012345678",
  "faulting_module": {
    "name":   "myDriver",
    "base":   "0xfffff80012340000",
    "offset": "0x5678"
  },
  "thread_id": 4
}
```

Field notes:

- `name` is resolved from a static built-in table (no symbols needed). If
  unknown (e.g. driver-specific bugchecks), `name` is the string `"UNKNOWN"`.
- `faulting_module` is resolved by `IDebugSymbols::GetModuleByOffset` over
  the bugcheck `ip`. If `ip` is unmapped or outside any module, the field is
  `null` (e.g. wild jumps).
- This event fires **once per bugcheck**. A second bugcheck within the same
  session (rare; usually the target is dead) emits another event.

### `break` — target halted

Fires whenever execution status transitions from a GO state to a BREAK state,
for any reason **except** bugcheck (those get the `bugcheck` event only — the
state-change is suppressed to avoid double-fires).

```jsonc
{
  "reason":      "interrupt",   // "interrupt" | "breakpoint" | "step" | "exception" | "load_complete" | "other"
  "ip":          "0xfffff800`12345678",
  "thread_id":   4,
  "prior_status":"GO",
  "current_status": "BREAK"
}
```

`reason` is best-effort and inferred from the immediately preceding callback:
a `Breakpoint` callback before the state change → `"breakpoint"`; a non-zero
`SetInterrupt` flag → `"interrupt"`; etc. When the cause is unclear, `"other"`.

### `breakpoint_hit` — specific breakpoint fired

Fires from the `Breakpoint` callback, **in addition** to the subsequent
`break` event. Useful when the AI wants the breakpoint id without parsing the
break reason.

```jsonc
{
  "bp_id":       3,
  "address":     "0xfffff80012345000",
  "expression":  "myDriver!DriverEntry",     // may be null
  "hit_count":   1
}
```

### `module_load` — driver / DLL loaded

```jsonc
{
  "name":      "myDriver",
  "image":     "myDriver.sys",
  "base":      "0xfffff80012340000",
  "size":      0x20000,
  "timestamp": 1709123456
}
```

### `module_unload` — driver / DLL unloaded

```jsonc
{
  "name": "myDriver",
  "base": "0xfffff80012340000"
}
```

### `session_status` — debugger session lifecycle

```jsonc
{
  "status": "active",   // "active" | "end" | "restart" | "reboot" | "hibernate" | "failure"
  "detail": "..."       // optional human-readable
}
```

Triggered by `IDebugEventCallbacks::SessionStatus`. `"end"` means the target
disconnected (KD link lost, dump closed); `"reboot"` is a guest reboot
observed via VirtualKD / KDNET.

### `state_change` — engine execution-status transition

Fires on any `ChangeEngineState(DEBUG_CES_EXECUTION_STATUS, ...)`. This is
the low-level signal underlying `break`, but the ext also publishes it raw
for callers that want every transition (e.g. step-over watchers).

```jsonc
{
  "from": "GO",
  "to":   "STEP_OVER"
}
```

## Status string mapping

`DEBUG_STATUS_*` ULONGs are mapped to text for AI legibility:

| ULONG value          | Text         |
|----------------------|--------------|
| `DEBUG_STATUS_NO_CHANGE` | `NO_CHANGE` |
| `DEBUG_STATUS_GO`        | `GO` |
| `DEBUG_STATUS_GO_HANDLED` | `GO_HANDLED` |
| `DEBUG_STATUS_GO_NOT_HANDLED` | `GO_NOT_HANDLED` |
| `DEBUG_STATUS_STEP_OVER` | `STEP_OVER` |
| `DEBUG_STATUS_STEP_INTO` | `STEP_INTO` |
| `DEBUG_STATUS_BREAK`     | `BREAK` |
| `DEBUG_STATUS_NO_DEBUGGEE`| `NO_DEBUGGEE` |
| `DEBUG_STATUS_STEP_BRANCH` | `STEP_BRANCH` |
| `DEBUG_STATUS_IGNORE_EVENT`| `IGNORE_EVENT` |
| `DEBUG_STATUS_RESTART_REQUESTED` | `RESTART_REQUESTED` |
| `DEBUG_STATUS_REVERSE_GO` | `REVERSE_GO` |
| `DEBUG_STATUS_REVERSE_STEP_BRANCH` | `REVERSE_STEP_BRANCH` |
| `DEBUG_STATUS_REVERSE_STEP_OVER` | `REVERSE_STEP_OVER` |
| `DEBUG_STATUS_REVERSE_STEP_INTO` | `REVERSE_STEP_INTO` |
| `DEBUG_STATUS_OUT_OF_SYNC` | `OUT_OF_SYNC` |
| `DEBUG_STATUS_WAIT_INPUT` | `WAIT_INPUT` |
| `DEBUG_STATUS_TIMEOUT`    | `TIMEOUT` |

## Coalescing rules

- **Bugcheck wins.** When a bugcheck and a regular `break` would fire from
  the same underlying event, only `bugcheck` is published.
- **Breakpoint pairs.** `breakpoint_hit` is published **before** the `break`
  that immediately follows it, so an AI waiting on `["break"]` will see the
  break and one waiting on `["breakpoint_hit","break"]` will see both in
  order.
- **No deduplication of module events.** Each load/unload is a distinct
  event even when many fire in quick succession (boot phase).

## What is NOT an event

These are deliberately *not* push events; the AI must poll if it cares:

- Register changes (too noisy; ask via `wm_run_cmd` `r`)
- Memory writes from user code (no callback for it)
- Symbol load completion (no callback; subsumed by `module_load`)
- Output to the WinDbg command window (would generate megabytes; capture
  per-command via the output callback during `run_cmd`)

## Subscription semantics summary

```python
# Typical AI flow after starting a test
wm_run_cmd("g")                            # let target run
event = wm_wait_event(
    kinds=["bugcheck", "break"],
    timeout_ms=120000,
    since_ms=now_ms - 5000                 # catch events from the last 5s
)
if event.kind == "bugcheck":
    report = wm_analyze_crash()            # AI's choice, not automatic
```
