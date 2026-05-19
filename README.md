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
                  │  │  - per-request worker threads  │  │
                  │  └──────────────┬─────────────────┘  │
                  └─────────────────┴────────────────────┘
                                    │  \\.\pipe\windbgmcp
                                    │  - 4B length + JSON payload
                                    │  - req / resp / event / chunk
                                    │  - single connection, multiplexed
                                    ▼
              ┌─────────────────────────────────────┐
              │  windbg-mcp.exe                     │
              │  - single async connection          │
              │  - request id → std::promise        │
              │  - event bus (with history replay)  │
              │  - 6 MCP tools                      │
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

Six tools, each with a single clear purpose. See `docs/tools.md` for full
signatures and example flows.

| Tool | Purpose |
|---|---|
| `wm_session` | snapshot of debugger state (`target_kind`, `exec_status`, `ip`, `bugcheck`, …) |
| `wm_run_cmd` | run any WinDbg command verbatim; optionally stream large output to disk |
| `wm_wait_event` | block until a debugger event arrives (bugcheck, break, module load, …) with 30 s history replay |
| `wm_break_in` | issue `SetInterrupt` and wait on the ext's own break event — no polling |
| `wm_analyze_crash` | structured BSOD report: `!analyze -v` + `kb` + `lm` + `!drvobj`, parsed into JSON |
| `wm_exit` | detach the dbgeng session |

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
pipe down. `!mcpext.help` lists the commands.

### 2. Register the MCP server with your AI client

For **Claude Code CLI** at user scope:
```powershell
claude mcp add --scope user windbg-mcp C:\tools\windbg-mcp\windbg-mcp.exe
```

For other MCP clients, point them at `windbg-mcp.exe` over stdio.

### 3. Drive the debugger from the AI

The AI can now call any of the six tools. A typical kernel-driver
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

`windbg-mcp.exe` keeps the AI-side stdio connection alive across:

- WinDbg restarts — next tool call re-opens the named pipe automatically
- `.reboot` against the kernel target — `mcpext.dll` is unloaded by
  dbgeng during `.reboot`, but the host transparently reconnects after
  the operator re-runs `.load mcpext` + `!mcpext.start`
- Spurious pipe disconnects — all pending requests are failed with
  `disconnected` and the next request triggers a reconnect

The host process only goes away when the AI client kills it (typical
on session exit) or when stdin is closed.

## Wire protocol

Length-prefixed JSON frames over a single full-duplex named pipe.

```
+------------------+----------------------------+
|  4 bytes         |  N bytes                   |
|  uint32 LE       |  UTF-8 JSON payload        |
|  payload length  |                            |
+------------------+----------------------------+
```

Four frame kinds: `req` / `resp` / `event` / `chunk`. Requests are
multiplexed by `id`; events are pushed asynchronously. Single instance
only — one WinDbg per host. See `docs/protocol.md` for the full spec
and error-code list.

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
    └── host/                     gtest: frame codec, bugcheck parser, stack parser
```

## Status

v1.0. The pipe transport, event push, all six tools, and the
`.reboot` lifecycle have been validated against live kernel-debug
sessions (VirtualKD + WinDbg Preview). Known limitations:

- single-instance only (one WinDbg per host); multi-instance support is
  a future extension to the protocol
- `wm_analyze_crash` parser was developed against Windows 10 / 11
  `!analyze -v` output; older versions may need parser updates

## License

MIT. See `LICENSE`.
