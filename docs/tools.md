# MCP tool surface

Eight tools, each with a single clear purpose. The AI's mental model:

1. **Is something there?** → `wm_session`
2. **Run a WinDbg command.** → `wm_run_cmd`
3. **Wait for something to happen.** → `wm_wait_event`
4. **Halt a running target.** → `wm_break_in`
5. **Got a BSOD — analyse it.** → `wm_analyze_crash`
6. **Detach the target but keep the bridge.** → `wm_detach`
7. **Stop the bridge but keep the target.** → `wm_shutdown`
8. **Support an old client.** → `wm_exit` (deprecated alias for `wm_detach`)

All tools return a JSON-serialised result inside the MCP `tools/call`
response. The host's worker thread blocks on the pipe with the
per-tool timeout; the AI sees a fully-formed reply or a structured
error.

---

## `wm_session`

Cheap snapshot of debugger state. Use as a liveness probe and to learn what
target you're attached to.

**Args:** none.

**Returns:**

```jsonc
{
  "attached":      true,
  "target_kind":   "kernel",        // "kernel" | "user" | "dump" | "none"
  "exec_status":   "BREAK",         // see docs/events.md status table
  "ip":            "0xfffff800`12345678",
  "thread_id":     4,
  "bugcheck":      null,            // or {code, name, params, ip, faulting_module}
  "modules_count": 324,
  "pipe_endpoint": "\\\\.\\pipe\\windbgmcp-project-a",
  "connection_generation": 7,
  "engine_lane":   "serial",
  "ext_version":   "2.0.0"
}
```

**Failure modes:** `disconnected`.

`pipe_endpoint` is useful when diagnosing a multi-project setup: it confirms
which extension instance answered. `connection_generation` confirms which
host connection produced the response and increases after every reconnect.
`engine_lane="serial"` means requests for this WinDbg instance execute FIFO on
one thread; separate endpoints can run in parallel.

For a live target whose `exec_status` is running, `wm_session` deliberately
does not query registers, the current thread, modules, or bugcheck memory over
KD. In that state `ip`, `thread_id`, and `modules_count` are zero and
`bugcheck` is `null`; call `wm_break_in` before requesting detailed context.

**Typical use:** after starting the harness, before driving any guest action,
poll once to confirm the ext is alive and find out if the previous run left
the target in a bugcheck state.

---

## `wm_run_cmd`

Execute UTF-8 WinDbg commands through `IDebugControl4::ExecuteWide`.
Top-level semicolons or actual newlines separate up to 64 sequential commands.
Quotes, expressions and control blocks stay intact. Inside a block, use normal
WinDbg semicolons; line-owning aliases, scripts and `*` comments retain their
native semantics. This is not a shell or a sandbox for arbitrary scripts.

**Args:**

| name           | type | default | meaning |
|----------------|------|---------|---------|
| `cmd`          | str  | —       | WinDbg command or top-level command batch; not an array or Markdown fence. |
| `timeout_ms`   | int  | 30000   | budget from 1 to 120000 ms; not hard cancellation of an in-flight command. |
| `output_file`  | str  | —       | absolute local path; the extension writes the full UTF-8 output, including partial failure output, after execution. |
| `preview_bytes`| int  | 8192    | head bytes, from 0 to 1048576; tail is 2048, aligned to UTF-8 boundaries. |

**Returns (success):**

```jsonc
{
  "ok":                   true,
  "commands_total":       2,
  "commands_executed":    2,
  "commands_skipped":     0,
  "results": [
    {"index": 0, "command": ".echo A", "status": "succeeded", "output": "A\n"},
    {"index": 1, "command": ".echo B", "status": "succeeded", "output": "B\n"}
  ],
  "output":               "...",          // head + tail with [...truncated N bytes...]
  "output_file":          "F:/...",       // only when caller set it
  "output_file_written":  true,           // only when caller set output_file
  "bytes_total":          123456,
  "truncated_in_response": true,          // false if full body fits and no file requested
  "exec_status_after":    "BREAK"
}
```

When the full body is ≤ 256 KiB and no `output_file` was requested, `output`
contains the whole thing and `truncated_in_response = false`. Above that
threshold, the response is truncated to the requested head plus a 2 KiB tail.
Set `output_file` before commands expected to produce large output; only repeat
a command after confirming that doing so is safe. Per-command previews retain
up to 1536 head + 512 tail bytes and have their own truncation flag.

The whole batch is validated before execution. Explicit `g`/step commands are
allowed only as the final standalone statement, never inside a block. `j`
command-string branches are rejected; use explicit `.if` blocks instead.
Lifecycle guards inspect statement heads, not quoted words being printed.
They cannot validate actions hidden in aliases, extension code or script files.

Execution stops after a failed HRESULT or `DEBUG_OUTPUT_ERROR`. The MCP reply
has `isError=true`, with `ok=false`, `err`, `failed_command_index`, partial
`output`, and `results` preserved. Later statements are `not_executed`; previous
effects are not rolled back. Ordinary debugger text such as unreadable-memory
`??` or warnings may not be errors, so inspect the evidence as well.

The deadline also covers work queued in the extension. Expired commands are
not started. A single DbgEng call cannot be safely hard-cancelled; when it
returns late its status is `completed_after_deadline` and the remaining batch
is skipped. The host allows 5 seconds of transport grace. If it still has no
reply, it returns `execution_state="unknown"`, `execution_may_continue=true`
and `safe_to_retry=false`. Wait for the debugger and inspect state instead of
blindly resubmitting. File errors preserve available output and report
`output_file_written=false` with `output_file_error`; do not assume a complete
log exists.

**Examples:**

```python
# simple inspection
wm_run_cmd(cmd="r rax")
# → {"output": "rax=0000000000000000", "bytes_total": 18, ...}

# multi-command in one call
wm_run_cmd(cmd="!process 0 0 explorer.exe\nlmvm explorer\n")

# big output to file
wm_run_cmd(
    cmd="!for_each_module .echo @#ModuleName",
    output_file="F:/dump/modules.txt"
)
# → {"output": "<first 8KiB>\n[...truncated 412 KiB...]\n<last 2KiB>",
#    "output_file": "F:/dump/modules.txt",
#    "bytes_total": 425984, "truncated_in_response": true, ...}
```

**Escaping reminder.** The `cmd` string is a JSON string — quotes, backslashes,
and newlines are encoded by the JSON layer. Literal backslash plus `n` is not
a newline and is not automatically unescaped. Do not double-escape. To run
`dt nt!_EPROCESS @$proc`, pass exactly `"dt nt!_EPROCESS @$proc"`.

**Failure modes:** `not_attached`, `target_running` (if the command requires
break state — most do), `timeout`, `engine_error`, `invalid_arg`.

Lifecycle-changing commands are intentionally not raw passthrough operations.
`!mcpext.*`, `.reboot`, `.unload`, and debugger quit commands return
`invalid_arg`; stop the bridge in WinDbg and issue those commands directly.

---

## `wm_wait_event`

Block until a matching debugger event arrives, with optional replay of recent
history. **This is the cure for polling.** If you're waiting for a BSOD or a
breakpoint, call this instead of looping on `wm_session`.

**Args:**

| name         | type      | default | meaning |
|--------------|-----------|---------|---------|
| `kinds`      | list[str] | —       | event kinds to match; see docs/events.md |
| `timeout_ms` | int       | 60000   | how long to wait; no internal cap |
| `since_ms`   | int       | —       | optional Unix ms; if set, replay matching events newer than this from the ext's 30s ring buffer |

**Valid `kinds`:** `bugcheck`, `break`, `breakpoint_hit`, `module_load`,
`module_unload`, `session_status`, `state_change`.

**Returns (success):** the matched event, exactly as it would appear on the
wire (envelope flattened):

```jsonc
{
  "kind": "bugcheck",
  "ts":   1729872345123,
  "data": { /* see docs/events.md */ }
}
```

**Failure modes:** `timeout` (no event arrived in window), `disconnected`,
`invalid_arg` (unknown kind).

**Typical patterns:**

```python
# wait for the next BSOD (no time bound)
ev = wm_wait_event(kinds=["bugcheck"], timeout_ms=300000)

# resume target then catch whichever happens first
wm_run_cmd(cmd="g")
ev = wm_wait_event(
    kinds=["bugcheck", "break"],
    timeout_ms=120000,
    since_ms=int(time.time() * 1000) - 5000   # look back 5s
)
# ev.kind tells us which side won

# specifically catch a breakpoint we just set
wm_run_cmd(cmd="bp myDriver!DriverEntry")
wm_run_cmd(cmd="g")
ev = wm_wait_event(kinds=["breakpoint_hit"], timeout_ms=60000)
```

---

## `wm_break_in`

Halt a running target. Internally issues `SetInterrupt(DEBUG_INTERRUPT_ACTIVE)`
from a freshly created `IDebugClient`, then **waits on the ext's own event
sink** for the `break` event — no polling. If the target was already broken
at entry, returns immediately.

**Args:**

| name         | type | default | meaning |
|--------------|------|---------|---------|
| `timeout_ms` | int  | 30000   | how long to wait for the break to land |

**Returns (success):**

```jsonc
{
  "ok":             true,
  "already_broken": false,
  "prior_status":   "GO",
  "current_status": "BREAK"
}
```

**Failure modes:** `not_attached`, `timeout` (interrupt sent but target
didn't halt in time — usually a hung guest), `engine_error`, `disconnected`.

**When to use:** before any inspection command (`k`, `r`, `dt`, …) that
requires the target to be at a known stop. After a `g`, this is your
companion call.

---

## `wm_analyze_crash`

The one-stop BSOD reporter. Server-side composition: runs `!analyze -v`,
`kb`, `lm m <faulting_module>`, and `!drvobj` if applicable; parses the
output into a structured report. No magic — the AI could do this manually,
but it's slow and the parse is fiddly.

Requires the target to be in a bugcheck state. If not, returns `not_attached`
or a clear error.

**Args:**

| name          | type | default | meaning |
|---------------|------|---------|---------|
| `output_file` | str  | —       | optional absolute path; raw text of all underlying commands is concatenated and written here. The structured report is still returned regardless. |

**Returns (success):**

```jsonc
{
  "bugcheck": {
    "code":    "0x000000D1",
    "name":    "DRIVER_IRQL_NOT_LESS_OR_EQUAL",
    "params": [
      "0xfffff80012345000",
      "0x0000000000000002",
      "0x0000000000000001",
      "0xfffff80012345678"
    ],
    "summary": "An attempt was made to access a pageable address at IRQL >= DISPATCH_LEVEL"
  },
  "faulting": {
    "ip":       "0xfffff800`12345678",
    "module":   "myDriver",
    "function": "MyDispatchRoutine",
    "offset":   "+0x48",
    "source":   null                  // file:line if symbols permit
  },
  "probable_culprit": {
    "module":    "myDriver.sys",
    "version":   "1.0.0.0",
    "timestamp": "Fri Mar 01 12:34:56 2026"
  },
  "stack": [
    {"frame":0, "addr":"0xfffff800`...", "module":"nt",       "func":"KeBugCheckEx",   "offset":"+0x107"},
    {"frame":1, "addr":"0xfffff800`...", "module":"nt",       "func":"KiBugCheckDispatch","offset":"+0x69"},
    {"frame":2, "addr":"0xfffff800`...", "module":"myDriver", "func":"MyDispatch",     "offset":"+0x48"}
  ],
  "iret_frame": {"rip":"...", "rsp":"...", "cs":"...", "ss":"...", "rflags":"..."},
  "context":    {"rax":"...", "rbx":"...", "rcx":"...", "rip":"...", "rsp":"..."},
  "raw": {
    "analyze": "<full !analyze -v text>",
    "kb":      "<kb output>",
    "lm":      "<lm m myDriver text>",
    "drvobj":  "<!drvobj text or null>"
  },
  "output_file": "F:/dump/bsod.txt"   // only when caller set it
}
```

Field-level guarantees:

- `bugcheck.code`, `bugcheck.name`, `bugcheck.params` are **always** present if
  the dbgeng reported a bugcheck.
- All other fields may be `null` when the underlying command failed (no
  symbols, kernel only, etc.). The `raw` section is the fallback — the AI can
  always read it.
- `output_file` contains the verbatim concatenation `===== !analyze -v ===== \n
  ... \n ===== kb ===== \n ...` and is intended for the human operator or
  longer-term storage, not for re-feeding to the AI.

**Failure modes:** `not_attached`, `target_running` (target must be halted at
the bugcheck), `timeout` (HEAVY tier = 120 s; raise if symbol download is
slow), `engine_error`.

**Typical use:**

```python
ev = wm_wait_event(kinds=["bugcheck"], timeout_ms=300000)
if ev["kind"] == "bugcheck":
    report = wm_analyze_crash(output_file="F:/dump/last_bsod.txt")
    # AI can read report["probable_culprit"] and report["stack"] directly
```

---

## `wm_detach`

Detach the debugger target only. The extension pipe, MCP host, and WinDbg UI
remain running. This operation does not mean “shut down the bridge.”

**Args:** none.

**Returns:**

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

**Failure modes:** `not_attached`, `engine_error`, `disconnected`. A failed
dbgeng detach is reported rather than converted into a false success.
User targets use `DetachProcesses`; kernel targets use
`EndSession(DEBUG_END_ACTIVE_DETACH)`. An image/dump session is passively
closed and returns `target_kind="image_file"` with `target_detached=false`.

---

## `wm_shutdown`

Stop the WinDbg extension bridge only. It does not detach, resume, terminate,
or otherwise change the target, and it does not close WinDbg or terminate the
stdio `windbg-mcp.exe` host.

**Args:** none.

**Returns:**

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

The success response is a terminal response for its connection generation.
The extension writes the complete length-prefixed response with a deadline and
waits for the host reader's bounded ACK, then fences that exact generation and
wakes a teardown executor prepared before startup committed. A stale
request from an older generation cannot shut down a replacement connection,
and the Router worker never joins itself or performs callback cleanup on its
dbgeng request stack.

Once the host has returned the MCP result, the named pipe disconnects. The
host stays alive but is pinned to the original `bridge_instance_id`; later
tool calls return `disconnected`. After a deliberate extension restart,
restart `windbg-mcp.exe` to bind the new instance even when the endpoint name
is unchanged. Requests queued behind `wm_shutdown` are failed as
`disconnected`; do not enqueue work after it.

**Failure modes:** `disconnected`. If the final response cannot be written and
ACKed on the requesting generation, teardown is deliberately suppressed.

---

## `wm_exit` (deprecated)

Compatibility alias for `wm_detach`. It does **not** stop the bridge or close
WinDbg. New callers must use the explicit operation that matches their intent.

**Args:** none.

**Returns:** the `wm_detach` result plus:

```jsonc
{
  "deprecated": true,
  "replacement": "wm_detach"
}
```

**Failure modes:** the same as `wm_detach`.

---

## Tools intentionally NOT provided

The old bridge had ~14 tools. The following are deliberately absent:

| Old tool                     | What to use instead |
|------------------------------|---------------------|
| `analyze_process / thread / memory / kernel` | `wm_run_cmd("!process ...")` etc. |
| `run_sequence`               | `wm_run_cmd("cmd1\ncmd2\n...")` |
| `performance_manager / async_manager` | nothing — single-connection multiplex covers it |
| `connection_manager / session_manager / debug_session` | `wm_session` |
| `troubleshoot / get_help`    | nothing — errors carry actionable `tip` strings |
| `breakpoint_and_continue`    | `wm_run_cmd("bp ...; g")` + `wm_wait_event` |
| `for_each_module / lm`       | `wm_run_cmd("lm m *")` |
| `health_check / performance_metrics` | `wm_session` |
| `execute_command_streaming`  | `wm_run_cmd(output_file=...)` |

The principle: one expressive primitive (`run_cmd`) beats ten specialised
wrappers when the AI knows the WinDbg command language already.

---

## Recommended AI workflow templates

### Trigger a BSOD and analyse it

```
1.  wm_session()                                     # confirm attached, get target_kind
2.  ... cause BSOD via harness ...
3.  ev = wm_wait_event(kinds=["bugcheck"], timeout_ms=120000)
4.  report = wm_analyze_crash()
5.  ... reason about report.probable_culprit ...
```

### Set a breakpoint and inspect on hit

```
1.  wm_break_in()                                    # ensure halted
2.  wm_run_cmd("bp myDriver!DriverEntry")
3.  wm_run_cmd("g")
4.  wm_wait_event(kinds=["breakpoint_hit"], timeout_ms=60000)
5.  wm_run_cmd("r; k; dt _DRIVER_OBJECT @rcx")
6.  wm_run_cmd("g")                                  # continue
```

### Sanity-check the harness without driving the guest

```
1.  wm_session()
2.  if not attached: bail with a clear message to the user
```
