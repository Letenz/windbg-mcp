# windbg-mcp

[English](./README.md) | **中文**

把 [WinDbg](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/) 接进 [MCP（Model Context Protocol）](https://modelcontextprotocol.io/) 的桥。
让 AI 编程助手（Claude Code、Cursor、Cline……）像人一样上手内核调试——
跑命令、下断点、`!analyze -v`、捕获 BSOD，你能干的它都能干。

```
                  ┌──────────────────────────────────────┐
                  │  WinDbg                              │
                  │  ┌────────────────────────────────┐  │
                  │  │ mcpext.dll                     │  │
                  │  │  - IDebugEventCallbacks 事件回调 │  │
                  │  │  - length-prefixed JSON 管道    │  │
                  │  │  - 多 worker 线程异步分派        │  │
                  │  └──────────────┬─────────────────┘  │
                  └─────────────────┴────────────────────┘
                                    │  \\.\pipe\windbgmcp
                                    │  - 4B 长度 + JSON 载荷
                                    │  - req / resp / event / chunk
                                    │  - 单连接多路复用
                                    ▼
              ┌─────────────────────────────────────┐
              │  windbg-mcp.exe                     │
              │  - 单异步连接                       │
              │  - request id → std::promise        │
              │  - 事件总线（含历史回放）            │
              │  - 6 个 MCP 工具                    │
              └─────────────────────────────────────┘
                                    │
                                    ▼  stdio MCP (JSON-RPC)
                              AI 客户端
```

两个二进制文件，运行时零额外依赖：

| 二进制 | 加载方 | 作用 |
|---|---|---|
| `mcpext.dll` | WinDbg (`.load mcpext`) | dbgeng 扩展；订阅调试器事件；处理管道请求 |
| `windbg-mcp.exe` | AI 客户端（作为 stdio MCP server） | 在 stdio 上跑 MCP JSON-RPC，背后通过命名管道与 ext 通信 |

CRT 全部静态链接，拷到任何 Windows 机器就能跑，**不需要装 VC++ 运行库**。

## MCP 工具

6 个工具，每个职责清晰单一。完整签名和示例工作流见 `docs/tools.md`。

| 工具 | 用途 |
|---|---|
| `wm_session` | 调试器状态快照（`target_kind`、`exec_status`、`ip`、`bugcheck`、……） |
| `wm_run_cmd` | 原样跑任意 WinDbg 命令；可选把大输出流式落盘 |
| `wm_wait_event` | 阻塞等待调试器事件（bugcheck、break、模块加载……），带 30 秒历史回放 |
| `wm_break_in` | 发 `SetInterrupt`，再等 ext 自己的 break 事件——**不再轮询** |
| `wm_analyze_crash` | 结构化 BSOD 报告：`!analyze -v` + `kb` + `lm` + `!drvobj` 解析成 JSON |
| `wm_exit` | 断开 dbgeng 会话 |

## 编译

需要：
- Visual Studio 2019 或 2022，装好 C++ 桌面工作负载
- Windows 10/11 SDK（合理近期的版本即可）
- CMake 3.20+

```powershell
git clone <repo> windbg-mcp
cd windbg-mcp
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：
- `build/ext/Release/mcpext.dll`
- `build/host/Release/windbg-mcp.exe`

### 编译选项

| CMake 选项 | 默认 | 说明 |
|---|---|---|
| `WINDBGMCP_BUILD_TESTS` | `ON` | 构建 GoogleTest 单元测试 |
| `WINDBGMCP_ENABLE_LOGGING` | `OFF` | 把 `OutputDebugString` + 落盘日志（`mcpext.log` / `windbg-mcp.log` 与二进制同目录）编进来。默认关——生产构建不应该每次工具调用都往使用者磁盘写东西。排障/开发时可以打开 |

开发/排障时打开日志：
```powershell
cmake -B build -S . -DWINDBGMCP_ENABLE_LOGGING=ON
cmake --build build --config Release
```

启用日志后，把 `WINDBGMCP_LOG=1` 设进 `windbg-mcp.exe` 的环境变量可以把级别从 `Info` 提到 `Trace`。

## 安装

没有 installer。两个二进制扔到喜欢的目录，原地运行就行。

典型布局：
```
C:\tools\windbg-mcp\
    mcpext.dll
    windbg-mcp.exe
```

## 使用

### 1. 在 WinDbg 里加载扩展

内核调试会话中：
```
0: kd> .load C:\tools\windbg-mcp\mcpext.dll
0: kd> !mcpext.start
windbgmcp: listening on \\.\pipe\windbgmcp
```

辅助命令：
- `!mcpext.status` 查看管道和连接状态
- `!mcpext.stop` 停止管道
- `!mcpext.help` 列出所有命令

### 2. 在 AI 客户端注册 MCP server

**Claude Code CLI**（user 作用域）：
```powershell
claude mcp add --scope user windbg-mcp C:\tools\windbg-mcp\windbg-mcp.exe
```

其他 MCP 客户端，把 `windbg-mcp.exe` 当成 stdio MCP server 配进去即可。

### 3. 让 AI 操作调试器

AI 现在可以调用 6 个工具中的任意一个。典型的内核驱动调试流程大致是：

```python
# 大致是 AI 视角下的调用：
wm_session()                                          # 确认已附加
wm_run_cmd("bp myDriver!DriverEntry")
wm_run_cmd("g")
wm_wait_event(kinds=["breakpoint_hit", "bugcheck"])   # 阻塞到命中
wm_run_cmd("r; k; dt _DRIVER_OBJECT @rcx")
# ... 一段时间后跑出 BSOD ...
report = wm_analyze_crash()                           # 拿到结构化 JSON
```

`wm_wait_event` **默认回看最近 10 秒的历史事件**——AI 在某个动作之后才订阅时不会错过刚发生的事件。

## 自动重连 / 生命周期

`windbg-mcp.exe` 在下列场景**保持 AI 端 stdio 连接不掉线**：

- WinDbg 重启 —— 下一次工具调用自动重开命名管道
- 内核 target `.reboot` —— `.reboot` 期间 dbgeng 会卸 `mcpext.dll`，等你重新 `.load mcpext + !mcpext.start` 后 host 自动接回去
- 偶发管道断开 —— 所有等待中的请求立刻以 `disconnected` 失败，下一次请求触发重连

host 进程只在 AI 客户端主动 kill 它（一般是关闭会话）或 stdin 关闭时退出。

## 线路协议

单个全双工命名管道，承载长度前缀的 JSON 帧。

```
+------------------+----------------------------+
|  4 字节          |  N 字节                    |
|  uint32 LE       |  UTF-8 JSON payload        |
|  payload 长度    |                            |
+------------------+----------------------------+
```

4 种帧：`req` / `resp` / `event` / `chunk`。请求通过 `id` 多路复用；事件异步推送。
当前**仅支持单实例**——一台 Host 一个 WinDbg。完整规范和错误码列表见 `docs/protocol.md`。

## 仓库结构

```
windbg-mcp/
├── CMakeLists.txt                顶层 CMake
├── README.md / README_zh.md      项目说明（英/中）
├── docs/
│   ├── protocol.md               管道帧规范、错误码、超时
│   ├── events.md                 事件目录 + 历史回放语义
│   └── tools.md                  MCP 工具表面、签名、典型工作流
├── ext/                          C++ WinDbg 扩展 DLL
│   ├── CMakeLists.txt
│   └── src/{ipc,events,handlers,util}
├── host/                         C++ MCP server (stdio + 管道 桥)
│   ├── CMakeLists.txt
│   └── src/{transport,mcp,tools,analysis,util}
└── tests/
    ├── ext/                      gtest：帧编解码、事件历史
    └── host/                     gtest：帧编解码、bugcheck 解析、栈解析
```

## 状态

v1.0。管道传输、事件推送、6 个工具、`.reboot` 生命周期已在真机内核调试会话
（VirtualKD + WinDbg Preview）上验证通过。已知限制：

- 仅支持单实例（一台 host 一个 WinDbg）；多实例支持作为协议未来扩展
- `wm_analyze_crash` 解析器基于 Windows 10/11 的 `!analyze -v` 输出开发；
  老版本可能需要补充解析规则

## License

MIT。见 `LICENSE`。
