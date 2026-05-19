# MCP tool surface

Six tools, each with a single clear purpose. The AI's mental model:

1. **Is something there?** → `wm_session`
2. **Run a WinDbg command.** → `wm_run_cmd`
3. **Wait for something to happen.** → `wm_wait_event`
4. **Halt a running target.** → `wm_break_in`
5. **Got a BSOD — analyse it.** → `wm_analyze_crash`
6. **Shut down.** → `wm_exit`

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
  "ext_version":   "1.0.0"
}
```

**Failure modes:** `disconnected`.

**Typical use:** after starting the harness, before driving any guest action,
poll once to confirm the ext is alive and find out if the previous run left
the target in a bugcheck state.

---

## `wm_run_cmd`

Raw passthrough to `IDebugControl::Execute`. Whatever the AI types here is
what dbgeng sees, byte-for-byte after JSON decoding.

**Args:**

| name           | type | default | meaning |
|----------------|------|---------|---------|
| `cmd`          | str  | —       | the WinDbg command. Multiple commands separated by `\n`. |
| `timeout_ms`   | int  | 30000   | hard ceiling. 5000 / 30000 / 120000 are the recommended tiers. |
| `output_file`  | str  | —       | absolute path; if set, output is streamed to disk and only a preview is returned. |
| `preview_bytes`| int  | 8192    | head bytes in the preview; tail is always 2048. |

**Returns (success):**

```jsonc
{
  "output":               "...",          // head + tail with [...truncated N bytes...]
  "output_file":          "F:/...",       // only when caller set it
  "bytes_total":          123456,
  "truncated_in_response": true,          // false if full body fits and no file requested
  "exec_status_after":    "BREAK"
}
```

When the full body is ≤ 256 KiB and no `output_file` was requested, `output`
contains the whole thing and `truncated_in_response = false`. Above that
threshold, the response is forcibly truncated to head 8 KiB + tail 2 KiB and
the AI is told to re-run with `output_file` if it wants everything.

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
and newlines are encoded by the JSON layer. Do not double-escape. To run
`dt nt!_EPROCESS @$proc`, pass exactly `"dt nt!_EPROCESS @$proc"`.

**Failure modes:** `not_attached`, `target_running` (if the command requires
break state — most do), `timeout`, `engine_error`, `invalid_arg`.

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

## `wm_exit`

Detach the debugger and shut down the ext's pipe server. WinDbg's UI is left
intact (closing the GUI is the human's job).

**Args:** none.

**Returns:** `{ "ok": true }`.

**Failure modes:** `disconnected` (already gone — treat as success).

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
