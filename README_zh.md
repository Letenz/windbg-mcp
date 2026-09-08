# windbg-mcp

[English](README.md)

连接 AI Agent 与 Windows 调试器的原生 MCP bridge。通过 `windbg-mcp.exe` 和
`mcpext.dll`，把调试器状态、命令执行、事件等待与崩溃分析开放为结构化工具。

它负责**调试器连接和取证**，不负责创建 VM、编译驱动或恢复 VMware 快照。
需要完整驱动测试流程时，使用上层 [windows-drv-harness](https://github.com/Letenz/windows-drv-harness)。

```text
AI / 自写 Agent
  -> stdio MCP -> windbg-mcp.exe
       -> 指定的本地命名管道
            -> mcpext.dll -> WinDbg / KD / CDB -> 调试目标
```

## 核心特性

| 特性 | 能力 |
|---|---|
| 原生 C++ bridge | 两个 x64 二进制，静态链接 CRT，不额外要求安装 VC++ Redistributable |
| 独立调试会话 | 每组调试器和 MCP host 使用独立 endpoint，支持多组会话同时运行 |
| 多条命令执行 | 分号或顶层换行分隔命令，保留引号、表达式和控制块的语义 |
| Unicode 输入输出 | 使用 DbgEng 宽字符接口，UTF-8 结果、文件和分块不截断字符 |
| 逐条结果与失败证据 | 返回每条命令状态、已执行/跳过数量，失败保留此前输出并停止后续顶层命令 |
| 大输出落盘 | 完整输出保存到指定文件，返回有截断标记的预览，写入失败不会伪报成功 |
| 结构化崩溃分析 | 组合 `!analyze -v`、栈和模块检查，提取 bugcheck、故障位置、访问类型等可用证据 |
| 调试事件等待 | 等待 bugcheck、断点和模块事件，支持近期历史回放，减少模型轮询 |
| 明确的生命周期 | 区分目标 detach、bridge shutdown 和 MCP host 退出，防止重连时串到其他实例 |

## 环境与编译

运行需要 Windows x64 和已安装的 Debugging Tools for Windows。`mcpext.dll` 加载在
调试器内，`windbg-mcp.exe` 由 MCP 客户端启动。静态 CRT 不代表无需安装调试器。

内核调试可使用经典 WinDbg 或 KD，用户态调试可使用 CDB。
VirtualKD/VMware 是内核实验环境的一种选择，不是 bridge 对所有调试目标的硬依赖。

编译需要支持 C++20 的 Visual Studio C++ 工具链、Windows SDK 和 CMake 3.20+。
下面使用 Visual Studio 2022；首次配置需要获取 CMake 声明的第三方依赖。

```powershell
git clone https://github.com/Letenz/windbg-mcp.git
cd windbg-mcp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

产物：

- `build/ext/Release/mcpext.dll`
- `build/host/Release/windbg-mcp.exe`

`WINDBGMCP_BUILD_TESTS` 默认开启，`WINDBGMCP_ENABLE_LOGGING` 默认关闭。需要诊断
bridge 本身时再启用内部日志；显式请求的命令输出文件不受这个编译开关影响。

## 接入 Agent

### 1. 在已配置的调试会话中加载扩展

将两个二进制放到固定目录，例如 `C:\tools\windbg-mcp`，在调试器命令窗口执行：

```text
.load C:\tools\windbg-mcp\mcpext.dll
!mcpext.start windbgmcp-lab-a
!mcpext.status
```

`!mcpext.start` 不带参数时使用默认 endpoint `windbgmcp`。自定义名称允许 ASCII
字母、数字、`.`、`_`、`-`，最长 240 字符；也接受完整的本地 `\\.\pipe\<name>`。

示例路径不含空格。自动启动调试器时，可把二进制目录作为工作目录，使用
`.load .\mcpext.dll`，避免嵌套命令行中的路径转义问题。

### 2. 注册 stdio MCP Server

在 MCP 客户端中配置下面的命令和参数；外层配置格式按客户端要求填写：

```json
{
  "command": "C:\\tools\\windbg-mcp\\windbg-mcp.exe",
  "args": ["--pipe", "windbgmcp-lab-a"]
}
```

host 和扩展必须选择同一个 endpoint。选择优先级是 `--pipe`、环境变量
`WINDBGMCP_PIPE`、默认值。连接后先调用 `wm_session`，确认目标类型、状态和
`pipe_endpoint`，不要仅凭进程存在判断连接成功。

多会话时，每个调试器实例搭配独立的 MCP host 和唯一 endpoint；每个 endpoint
同时接受一个 host 连接。该 bridge 不替代 WinDbg/KD 本身的目标连接配置。

## MCP 工具

七个主要工具，加一个弃用兼容别名。完整参数见 [工具文档](docs/tools.md)。

| 工具 | 职责 |
|---|---|
| `wm_session` | 查看目标类型、执行状态、连接 endpoint，以及可获得的 bugcheck/上下文 |
| `wm_run_cmd` | 执行命令批次，返回逐条结果、输出和可选完整日志 |
| `wm_wait_event` | 等待调试事件；默认回看最近 10 秒的可用历史 |
| `wm_break_in` | 暂停运行中的目标，等待 break；已暂停时直接返回 |
| `wm_analyze_crash` | 返回结构化崩溃报告及原始诊断文本 |
| `wm_detach` | 分离调试目标，保留 bridge 和 MCP host |
| `wm_shutdown` | 响应完成后停止 bridge，不改变目标挂接状态，不退出 MCP host |
| `wm_exit` | `wm_detach` 的弃用兼容别名，不表示关闭全部进程 |

与上层 Harness 的 `debug_run` 不同，原生 `wm_run_cmd` 不会自动暂停目标。
运行中的目标应先调用 `wm_break_in`。以下是工具调用示意，不是 Python SDK：

```python
wm_session()
wm_break_in()
wm_run_cmd(cmd="vertarget; r\nlm m nt")
```

## 批量命令与结果

`cmd` 是一个 UTF-8 字符串，不是数组、shell 脚本或 Markdown 代码块。

- 最多 64 条顶层命令，以分号或实际换行分隔；字面量反斜杠加 `n` 不会自动解转义。
- 引号、表达式和控制块保持完整；控制块内仍使用 WinDbg 的分号规则。
- 别名、脚本命令和 `*` 注释等消费整行的语法，仍保留 WinDbg 原生行为。
- `g`、步进等运行控制只能是最后一条独立命令，不能放在批次中间或控制块里。
- 不支持 `j` 的命令字符串分支，改用显式 `.if` 控制块。
- 整批先做结构校验；某条命令返回失败 HRESULT 或错误输出后，后续顶层命令跳过。

| 结果字段 | 含义 |
|---|---|
| `ok` / MCP `isError` | 执行结果；收到响应不代表命令成功 |
| `results` | 每条命令的索引、状态及可用的输出预览 |
| `commands_executed` / `commands_skipped` | 实际执行数量与跳过数量 |
| `failed_command_index` | 失败命令位置（适用时） |
| `output` / `bytes_total` / `truncated_in_response` | 合并输出、完整字节数和预览截断标记 |
| `output_file_written` / `output_file_error` | 指定输出文件的写入结果 |

失败响应仍保留已有输出。单条状态包括 `succeeded`、`failed`、
`completed_after_deadline` 和 `not_executed`。缺失扩展不会仅因 API 返回成功就被
当成执行成功；普通警告或不可读内存的 `??` 仍需结合实际内容判断。

### 大输出与超时

为大输出事先指定 `output_file`，使用父目录已存在的本机绝对路径。扩展在执行后
写入完整 UTF-8 内容，包含已获得的失败输出；返回值只保留预览。不要为了补日志
盲目重跑有副作用的命令。

`timeout_ms` 是执行预算，范围 1 到 120000 ms，不是对 DbgEng 的硬取消承诺。
过期的排队命令不再启动；单条命令超时返回后，剩余顶层命令跳过。

如果 host 等待预算及 5 秒宽限后仍未收到结果，会返回
`execution_state="unknown"`、`execution_may_continue=true` 和
`safe_to_retry=false`。**正在执行的命令可能继续，已发生的修改不会回滚。**
先等待调试器恢复响应并检查状态，不要直接重发整个批次。

## 生命周期与隔离

`wm_detach` 只处理目标，`wm_shutdown` 只停止 bridge。要结束 stdio MCP host，
由客户端关闭或终止对应的 `windbg-mcp.exe`，不要把 shutdown 当作进程退出。

同一个扩展实例的临时断线可以重连。扩展重新启动后，即使沿用同名 endpoint，也会
生成新的 `bridge_instance_id`；host 不会自动绑定新实例，需要重启 MCP host。
旧连接的排队请求和迟到响应不会被转交给替代连接。

`wm_run_cmd` 会拒绝 `!mcpext.*`、`.unload`、`.reboot` 和调试器退出等生命周期
命令，避免工作线程还在执行时卸载其代码。需要时使用专用工具，或在调试器中执行
`!mcpext.stop` 后再进行人工操作。

这些检查不是任意调试脚本的安全沙箱，无法验证藏在别名、脚本文件或扩展中的全部
动作。只向可信 Agent 开放自己的调试环境。

## 示例

[Harness 的 HelloWorld 示例](https://github.com/Letenz/windows-drv-harness/tree/main/example/HelloWorld)
使用本 bridge 完成驱动调试和取证，已使用 `qwen3.8-flash` 和 `glm-5.3-flash` 跑通。

## 使用限制

- 单个调试器 endpoint 内请求串行执行，长命令或事件等待会延迟后续请求；并行能力来自独立会话。
- 崩溃解析取决于符号、调试器输出和目标信息，字段可能缺失。原始文本和日志是核对依据。
- 控制块仍由 DbgEng 执行，逐条停止和超时边界针对顶层命令，不代表能中断控制块内部的每一步。

更多细节见 [工具文档](docs/tools.md)、[事件语义](docs/events.md)和[管道协议](docs/protocol.md)。

## License

MIT。第三方依赖保留各自许可证。
