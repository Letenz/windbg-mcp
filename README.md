# windbg-mcp

[中文说明](README_zh.md)

A native MCP bridge between AI agents and Windows debuggers. `windbg-mcp.exe`
and `mcpext.dll` expose debugger state, command execution, event waits and
crash analysis as structured tools.

This project handles **debugger access and evidence**, not VM creation,
driver builds or VMware snapshot recovery. For a complete driver-test workflow,
use [windows-drv-harness](https://github.com/Letenz/windows-drv-harness).

```text
AI / custom agent
  -> stdio MCP -> windbg-mcp.exe
       -> selected local named pipe
            -> mcpext.dll -> WinDbg / KD / CDB -> debug target
```

## Features

| Feature | What It Provides |
|---|---|
| Native C++ bridge | Two x64 binaries with a statically linked CRT; no additional VC++ Redistributable installation |
| Independent sessions | A unique endpoint per debugger/host pair, with multiple pairs operating together |
| Command batches | Semicolon or top-level newline separators while preserving quotes, expressions and control blocks |
| Unicode I/O | Wide-character DbgEng APIs and UTF-8 results/files with character-safe chunk boundaries |
| Per-command evidence | Execution status, executed/skipped counts and retained output; errors stop later top-level commands |
| Large output files | Full output written to a requested file, explicit preview truncation and file-write failures |
| Structured crash analysis | Compose `!analyze -v`, stack and module checks into available bugcheck, fault-location and access evidence |
| Debugger event waits | Wait for bugchecks, breakpoints and module events with recent-history replay |
| Explicit lifecycle | Separate target detach, bridge shutdown and MCP host exit; pin connections to the selected bridge instance |

## Requirements and Build

Runtime requires Windows x64 and installed Debugging Tools for Windows.
`mcpext.dll` runs inside the debugger; the MCP client launches `windbg-mcp.exe`.
A static CRT does not remove the debugger requirement.

Use classic WinDbg or KD for kernel debugging and CDB for user-mode debugging.
VirtualKD/VMware is one kernel-lab configuration, not a bridge requirement
for every kind of target.

Build with a C++20-capable Visual Studio C++ toolchain, Windows SDK and CMake
3.20+. The example uses Visual Studio 2022. Initial CMake configuration fetches
the declared third-party dependencies.

```powershell
git clone https://github.com/Letenz/windbg-mcp.git
cd windbg-mcp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Outputs:

- `build/ext/Release/mcpext.dll`
- `build/host/Release/windbg-mcp.exe`

`WINDBGMCP_BUILD_TESTS` defaults to `ON`; `WINDBGMCP_ENABLE_LOGGING` defaults
to `OFF`. Enable internal logging to diagnose the bridge itself. Explicitly
requested command-output files work independently of that build option.

## Connect an Agent

### 1. Load the Extension in a Configured Debugger Session

Put both binaries in a stable directory, such as `C:\tools\windbg-mcp`, then
run these commands in the debugger:

```text
.load C:\tools\windbg-mcp\mcpext.dll
!mcpext.start windbgmcp-lab-a
!mcpext.status
```

With no argument, `!mcpext.start` selects the default endpoint `windbgmcp`.
Custom names permit ASCII letters, digits, `.`, `_` and `-`, up to 240
characters. A full local `\\.\pipe\<name>` endpoint is also accepted.

The example path contains no spaces. For automated debugger startup, using
the binary directory as the working directory and `.load .\mcpext.dll`
avoids nested command-line path escaping.

### 2. Register the Stdio MCP Server

Configure this command and argument list in the MCP client, using its
required outer configuration format:

```json
{
  "command": "C:\\tools\\windbg-mcp\\windbg-mcp.exe",
  "args": ["--pipe", "windbgmcp-lab-a"]
}
```

The host and extension must select the same endpoint. Selection precedence
is `--pipe`, then `WINDBGMCP_PIPE`, then the default. Call `wm_session` after
connecting to verify the target type, execution state and `pipe_endpoint`;
a running process alone does not establish a working debug connection.

For multiple sessions, use a separate host and unique endpoint for each
debugger instance. Each endpoint accepts one host connection at a time.
The bridge does not replace the debugger's own target-connection setup.

## MCP Tools

Seven primary tools plus one deprecated compatibility alias. Full parameters
are in the [tool reference](docs/tools.md).

| Tool | Purpose |
|---|---|
| `wm_session` | Target type, execution state, endpoint and available bugcheck/context details |
| `wm_run_cmd` | Command batches, per-command results, output and optional full logs |
| `wm_wait_event` | Wait for debugger events; by default, look back over the last 10 seconds of available history |
| `wm_break_in` | Break into a running target and wait for the break; return immediately if already paused |
| `wm_analyze_crash` | Structured crash report with raw diagnostic text |
| `wm_detach` | Detach the target while keeping the bridge and MCP host alive |
| `wm_shutdown` | Stop the bridge after response delivery; keep target attachment and the MCP host |
| `wm_exit` | Deprecated alias for `wm_detach`, not a command to close all processes |

Unlike the Harness's high-level `debug_run`, native `wm_run_cmd` does not
automatically pause the target. Call `wm_break_in` first if it is running.
These are conceptual tool calls, not a Python SDK:

```python
wm_session()
wm_break_in()
wm_run_cmd(cmd="vertarget; r\nlm m nt")
```

## Batches and Results

`cmd` is one UTF-8 string, not an array, shell script or Markdown code fence.

- Up to 64 top-level commands use semicolons or actual newlines. Literal backslash plus `n` is not automatically unescaped.
- Quoted strings, expressions and control blocks stay intact. Blocks still use WinDbg's semicolon rules.
- Line-owning aliases, script commands and `*` comments retain their native consume-the-rest-of-line behavior.
- Run-control commands such as `g` and stepping must be the final standalone statement, never in the middle or inside a block.
- `j` command-string branches are unsupported here; use explicit `.if` blocks.
- Structural validation precedes execution. A failed HRESULT or debugger error output stops subsequent top-level commands.

| Field | Meaning |
|---|---|
| `ok` / MCP `isError` | Execution result; receiving a response is not proof of success |
| `results` | Each command's index, status and available output preview |
| `commands_executed` / `commands_skipped` | Actual execution and skip counts |
| `failed_command_index` | Failure position when applicable |
| `output` / `bytes_total` / `truncated_in_response` | Combined output, full byte count and preview-truncation flag |
| `output_file_written` / `output_file_error` | Requested output-file write outcome |

Errors retain earlier output. Per-command states include `succeeded`,
`failed`, `completed_after_deadline` and `not_executed`. A missing extension
is not treated as success just because the API returned successfully.
Ordinary warnings and unreadable-memory `??` still require interpretation.

### Large Output and Timeouts

Set `output_file` before commands expected to produce large output. Use an
absolute local path whose parent directory exists. The extension writes full
UTF-8 content after execution, including available failure output, and returns
a preview. Do not blindly repeat side-effecting commands merely to obtain a log.

`timeout_ms` is an execution budget from 1 to 120000 ms, not a promise to
hard-cancel DbgEng. Expired queued commands are not started. If an individual
command returns after its deadline, remaining top-level commands are skipped.

If the host receives no result after the budget plus 5 seconds of grace,
it returns `execution_state="unknown"`, `execution_may_continue=true` and
`safe_to_retry=false`. **An in-flight command may continue; prior effects are
not rolled back.** Wait for the debugger to respond and inspect state before
submitting further work.

## Lifecycle and Isolation

`wm_detach` affects the target; `wm_shutdown` stops only the bridge. To end
the stdio MCP host, the client must close or terminate its `windbg-mcp.exe`.
Do not treat bridge shutdown as process exit.

Transient disconnects can reconnect to the same extension instance. A newly
started extension has a new `bridge_instance_id`, even on the same endpoint.
Restart the MCP host to bind it. Queued requests and late responses from an
old connection are not transferred to its replacement.

`wm_run_cmd` rejects lifecycle commands such as `!mcpext.*`, `.unload`,
`.reboot` and debugger quit, preventing extension unload on an active worker
stack. Use dedicated tools, or stop the bridge with `!mcpext.stop` in the
debugger before manual lifecycle operations.

These checks are not a sandbox for arbitrary debugger scripts. They cannot
validate every action hidden in aliases, script files or extensions. Expose
only your own debugging environment to trusted agents.

## Example

The [Harness HelloWorld example](https://github.com/Letenz/windows-drv-harness/tree/main/example/HelloWorld)
uses this bridge for driver debugging and evidence collection. Its workflow
has been completed successfully using `qwen3.8-flash` and `glm-5.3-flash`.

## Usage Limits

- Requests within one debugger endpoint execute serially. Long commands or event waits delay later work; parallelism is across separate sessions.
- Crash parsing depends on symbols, debugger output and target information. Fields may be unavailable; retain raw text and logs for verification.
- DbgEng executes control blocks internally. Per-command stop/deadline handling applies to top-level statements, not every step inside a block.

See the [tool reference](docs/tools.md), [event semantics](docs/events.md) and
[pipe protocol](docs/protocol.md) for details.

## License

MIT. Third-party dependencies retain their own licenses.
