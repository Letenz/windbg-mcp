# windbg-mcp

**English** | [中文](./README_zh.md)

A Model Context Protocol (MCP) bridge for [WinDbg](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/).
Plug an AI coding assistant (Claude Code, Cursor, Cline, …) into a live
kernel-debug session and let it drive WinDbg the same way you would —
inspect threads, set breakpoints, run `!analyze -v`, react to BSODs, etc.

```
                  ┌──────────────────────────────────────┐
                  │  WinDbg                              │
                  │  ┌────────────────────────────────┐  │
                  │  │ mcpext.dll                     │  │
                  │  │  - IDebugEventCallbacks sink   │  │
                  │  │  - length-prefixed JSON pipe   │  │
                  │  │  - serial dbgeng request lane  │  │
                  │  └──────────────┬─────────────────┘  │
                  └─────────────────┴────────────────────┘
                                    │  selected \\.\pipe\<name>
                                    │  - 4B length + JSON payload
                                    │  - hello / req / resp / ack / event / chunk
                                    │  - single connection, multiplexed
                                    ▼
              ┌─────────────────────────────────────┐
              │  windbg-mcp.exe                     │
              │  - single async connection          │
              │  - request id → std::promise        │
              │  - event bus (with history replay)  │
              │  - 8 MCP tools                      │
              └─────────────────────────────────────┘
                                    │
                                    ▼  stdio MCP (JSON-RPC)
                              AI client
```

Two binaries, zero runtime dependencies beyond the OS:

| Binary | Loaded by | Role |
|---|---|---|
| `mcpext.dll` | WinDbg (`.load mcpext`) | dbgeng extension; subscribes to debugger events; serves pipe requests |
| `windbg-mcp.exe` | AI client (stdio MCP server) | speaks MCP JSON-RPC on stdio, talks to the ext over the named pipe |

Both are statically linked against the CRT, so you can copy them to any
Windows host without installing the VC++ Redistributable.

## MCP tools

Eight tools, each with a single clear purpose. See `docs/tools.md` for full
signatures and example flows.

| Tool | Purpose |
|---|---|
| `wm_session` | snapshot of debugger state (`target_kind`, `exec_status`, `ip`, `bugcheck`, …) |
| `wm_run_cmd` | run any WinDbg command verbatim; optionally stream large output to disk |
| `wm_wait_event` | block until a debugger event arrives (bugcheck, break, module load, …) with 30 s history replay |
| `wm_break_in` | issue `SetInterrupt` and wait on the ext's own break event — no polling |
| `wm_analyze_crash` | structured BSOD report: `!analyze -v` + `kb` + `lm` + `!drvobj`, parsed into JSON |
| `wm_detach` | detach the target only; keep the bridge and host running |
| `wm_shutdown` | stop the bridge after its response is delivered; leave the target attached |
| `wm_exit` | deprecated compatibility alias for `wm_detach` |

## Build

Requires:
- Visual Studio 2019 or 2022 with the C++ desktop workload
- Windows 10/11 SDK (any reasonably recent version)
- CMake 3.20+

```powershell
git clone <repo> windbg-mcp
cd windbg-mcp
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs:
- `build/ext/Release/mcpext.dll`
- `build/host/Release/windbg-mcp.exe`

### Build options

| CMake option | Default | What it does |
|---|---|---|
| `WINDBGMCP_BUILD_TESTS` | `ON` | build GoogleTest-based unit tests |
| `WINDBGMCP_ENABLE_LOGGING` | `OFF` | compile in `OutputDebugString` + on-disk logs (`mcpext.log` / `windbg-mcp.log` next to each binary). Off by default — production builds should not write to the operator's disk on every tool call |

Enable logging for development / triage:
```powershell
cmake -B build -S . -DWINDBGMCP_ENABLE_LOGGING=ON
cmake --build build --config Release
```

With logging enabled, set `WINDBGMCP_LOG=1` in `windbg-mcp.exe`'s environment
to bump the level from `Info` to `Trace`.

## Install

There's no installer. Drop the two binaries wherever you like; they run
in place.

A typical layout:
```
C:\tools\windbg-mcp\
    mcpext.dll
    windbg-mcp.exe
```

## Use

### 1. Load the WinDbg extension

In a kernel-debug session:
```
0: kd> .load C:\tools\windbg-mcp\mcpext.dll
0: kd> !mcpext.start
windbgmcp: listening on \\.\pipe\windbgmcp
```

`!mcpext.status` shows pipe + connection state. `!mcpext.stop` shuts the
pipe down. Status also reports the active connection generation and callback
owner thread. `!mcpext.help` lists the commands.

To give this WinDbg instance its own endpoint, pass either a short pipe name
or a full local endpoint:

```text
0: kd> !mcpext.start windbgmcp-project-a
windbgmcp: listening on \\.\pipe\windbgmcp-project-a
```

Names may contain ASCII letters, digits, `.`, `_`, and `-` (maximum 240
characters). The no-argument form remains backward compatible and selects
`\\.\pipe\windbgmcp`.

### 2. Register the MCP server with your AI client

For **Claude Code CLI** at user scope:
```powershell
claude mcp add --scope user windbg-mcp C:\tools\windbg-mcp\windbg-mcp.exe
```

For other MCP clients, point them at `windbg-mcp.exe` over stdio.

The host must select the same endpoint as the extension. Selection precedence
is `--pipe`, then `WINDBGMCP_PIPE`, then the default:

```powershell
C:\tools\windbg-mcp\windbg-mcp.exe --pipe windbgmcp-project-a
$env:WINDBGMCP_PIPE = "windbgmcp-project-a"
C:\tools\windbg-mcp\windbg-mcp.exe
```

For parallel projects, start one WinDbg + extension and one MCP host per
project, with a unique pipe name for each pair. Each endpoint still accepts
one host connection.

### Safe teardown

The event sink owns a dedicated dbgeng client created with `DebugCreate` on
its owner actor. Startup is asynchronous because a WinDbg extension command
already holds an internal engine lock; waiting there for another thread to
enter dbgeng would deadlock. Callback pumping is non-blocking and shares the
same engine coordinator as the serial request lane. Use `wm_shutdown` for a
remote bridge-only stop, or run `!mcpext.stop` directly in WinDbg before
unloading the extension or rebooting the target.
`wm_run_cmd` deliberately rejects
`!mcpext.*`, `.reboot`,
`.unload`, and debugger quit commands because executing them on the MCP worker
could unload code that is still on that worker's stack.

`DebugExtensionCanUnload` is the hard safety barrier behind that usability
guard. It returns `S_FALSE` while lifecycle state is not `Stopped`, or while a
Router worker, callback actor, async event publisher, or teardown is
outstanding. `!mcpext.stop`
moves the session resources under the lifecycle mutex, schedules a reaper,
and returns without joining dbgeng threads from inside the extension command.
The reaper closes the pipe, removes callbacks on their owner thread, joins the
request worker, and finally publishes `Stopped`. For `wm_shutdown`, the
extension performs a bounded terminal response/host-ACK exchange on the
requesting connection generation, atomically fences that generation, and only
then arms a teardown executor prepared before startup committed. The stdio MCP
host stays alive but remains pinned to that bridge instance; restart the host
to bind a deliberately restarted extension, even with the same endpoint name.

### 3. Drive the debugger from the AI

The AI can now call any of the eight tools. A typical kernel-driver
debugging session might look like:

```python
# AI's view, conceptually:
wm_session()                                          # confirm attached
wm_run_cmd("bp myDriver!DriverEntry")
wm_run_cmd("g")
wm_wait_event(kinds=["breakpoint_hit", "bugcheck"])   # blocks until hit
wm_run_cmd("r; k; dt _DRIVER_OBJECT @rcx")
# ... eventually a BSOD fires ...
report = wm_analyze_crash()                           # structured JSON
```

`wm_wait_event` defaults to a 10-second history replay so the AI doesn't
miss an event that fired while it was mid-call.

## Reconnect / lifecycle

`windbg-mcp.exe` keeps the AI-side stdio connection alive across transient
disconnects of the same extension instance:

- Spurious pipe disconnects — all pending requests are failed with
  `disconnected` and the next request triggers a reconnect

Every `!mcpext.start` sends a server-first `hello` with a fresh
`bridge_instance_id`. The host pins the first value, so an extension reload,
WinDbg restart, or another WinDbg reusing the same endpoint is rejected.
Restart `windbg-mcp.exe` to explicitly bind the new instance.

Every accepted connection receives a monotonic generation. Queued work from a
disconnected generation is dropped, and its responses/chunks cannot be sent to
a replacement client. The extension keeps the same server pipe handle open
between clients, so another process cannot claim the endpoint in the reconnect
window.

## Wire protocol

Length-prefixed JSON frames over a single full-duplex named pipe.

```
+------------------+----------------------------+
|  4 bytes         |  N bytes                   |
|  uint32 LE       |  UTF-8 JSON payload        |
|  payload length  |                            |
+------------------+----------------------------+
```

Six frame kinds: `hello` / `req` / `resp` / `ack` / `event` / `chunk`. Requests are
multiplexed by `id`; events are pushed asynchronously. Multiple WinDbg/host
pairs can coexist when each pair uses a unique endpoint. See
`docs/protocol.md` for the full spec and error-code list.

## Repository layout

```
windbg-mcp/
├── CMakeLists.txt                top-level CMake
├── README.md / README_zh.md      this file (English / Chinese)
├── docs/
│   ├── protocol.md               pipe frame spec, error codes, timeouts
│   ├── events.md                 event catalogue + history-replay semantics
│   └── tools.md                  MCP tool surface, signatures, workflows
├── ext/                          C++ WinDbg extension DLL
│   ├── CMakeLists.txt
│   └── src/{ipc,events,handlers,util}
├── host/                         C++ MCP server (stdio + pipe bridge)
│   ├── CMakeLists.txt
│   └── src/{transport,mcp,tools,analysis,util}
└── tests/
    ├── ext/                      gtest: codec, event history
    └── host/                     gtest: frame codec, parsers, endpoint/options
```

## Status

v2.0. The pipe transport, event push, core debugging operations, and the
`.reboot` lifecycle have been validated against live kernel-debug sessions
(VirtualKD + WinDbg Preview). Known limitations:

- requests within one WinDbg instance execute FIFO on a single dbgeng worker;
  this intentionally trades same-session request parallelism for correct
  `IDebugClient` thread affinity. A long `wm_wait_event` therefore delays
  later requests on that same endpoint. Parallelism is across distinct
  endpoints
- `wm_analyze_crash` parser was developed against Windows 10 / 11
  `!analyze -v` output; older versions may need parser updates

## License

MIT. See `LICENSE`.
