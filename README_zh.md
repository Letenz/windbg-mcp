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
                  │  │  - dbgeng 单线程串行请求通道     │  │
                  │  └──────────────┬─────────────────┘  │
                  └─────────────────┴────────────────────┘
                                    │  选定的 \\.\pipe\<name>
                                    │  - 4B 长度 + JSON 载荷
                                    │  - hello / req / resp / ack / event / chunk
                                    │  - 单连接多路复用
                                    ▼
              ┌─────────────────────────────────────┐
              │  windbg-mcp.exe                     │
              │  - 单异步连接                       │
              │  - request id → std::promise        │
              │  - 事件总线（含历史回放）            │
              │  - 8 个 MCP 工具                    │
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

8 个工具，每个职责清晰单一。完整签名和示例工作流见 `docs/tools.md`。

| 工具 | 用途 |
|---|---|
| `wm_session` | 调试器状态快照（`target_kind`、`exec_status`、`ip`、`bugcheck`、……） |
| `wm_run_cmd` | 原样跑任意 WinDbg 命令；可选把大输出流式落盘 |
| `wm_wait_event` | 阻塞等待调试器事件（bugcheck、break、模块加载……），带 30 秒历史回放 |
| `wm_break_in` | 发 `SetInterrupt`，再等 ext 自己的 break 事件——**不再轮询** |
| `wm_analyze_crash` | 结构化 BSOD 报告：`!analyze -v` + `kb` + `lm` + `!drvobj` 解析成 JSON |
| `wm_detach` | 只 detach target，bridge 和 host 继续运行 |
| `wm_shutdown` | response 发出后停止 bridge，不 detach target |
| `wm_exit` | `wm_detach` 的弃用兼容别名 |

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

状态还会显示当前 connection generation 和 callback owner 线程。

要给当前 WinDbg 实例分配独立 endpoint，可传短管道名或完整本地 endpoint：

```text
0: kd> !mcpext.start windbgmcp-project-a
windbgmcp: listening on \\.\pipe\windbgmcp-project-a
```

名称只允许 ASCII 字母、数字、`.`、`_`、`-`，最长 240 字符。不带参数的
调用仍兼容旧行为，使用 `\\.\pipe\windbgmcp`。

### 2. 在 AI 客户端注册 MCP server

**Claude Code CLI**（user 作用域）：
```powershell
claude mcp add --scope user windbg-mcp C:\tools\windbg-mcp\windbg-mcp.exe
```

其他 MCP 客户端，把 `windbg-mcp.exe` 当成 stdio MCP server 配进去即可。

host 必须选择与扩展相同的 endpoint。选择优先级依次为 `--pipe`、
`WINDBGMCP_PIPE`、默认值：

```powershell
C:\tools\windbg-mcp\windbg-mcp.exe --pipe windbgmcp-project-a
$env:WINDBGMCP_PIPE = "windbgmcp-project-a"
C:\tools\windbg-mcp\windbg-mcp.exe
```

多个项目并行时，每个项目启动一组 WinDbg + 扩展 + MCP host，并给每组分配
唯一管道名。每个 endpoint 仍只接收一个 host 连接。

### 安全停止

事件 sink 在专用 owner actor 中通过 `DebugCreate` 创建线程绑定的 dbgeng client。
初始化必须异步进行：WinDbg 执行扩展命令时已持有内部 engine lock，如果此时等待
另一个线程进入 dbgeng 会形成死锁。callback 使用非阻塞泵，并与串行 request lane
共用 engine coordinator。远程只停止 bridge 时使用 `wm_shutdown`；卸载扩展或
重启 target 前，也可以直接在 WinDbg 中执行 `!mcpext.stop`。
`wm_run_cmd` 会拒绝 `!mcpext.*`、`.reboot`、`.unload` 和退出
调试器命令，防止 MCP worker 尚在执行时同步卸载其代码。

真正的安全边界是 `DebugExtensionCanUnload`：只要 lifecycle 尚未进入
`Stopped`，或仍有 Router worker、callback actor、异步事件 publisher、teardown 活动，它就返回
`S_FALSE`。`!mcpext.stop` 只在 lifecycle mutex 内转移状态和资源，随后调度
reaper 并立即返回，避免在扩展命令持有 engine lock 时 join dbgeng 线程。reaper
负责关闭管道、在 owner 线程卸载 callback、join request worker，最后发布
`Stopped`。对于 `wm_shutdown`，扩展会在发起请求的 connection generation 上执行
有界的 terminal response/host ACK 交换，再原子封闭该 generation，最后唤醒启动
提交前已预建的 teardown executor。stdio MCP host 不会退出，但会固定到原 bridge
instance；扩展重新启动后即使复用同名 endpoint，也要重启 host 才能显式绑定。

### 3. 让 AI 操作调试器

AI 现在可以调用 8 个工具中的任意一个。典型的内核驱动调试流程大致是：

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

`windbg-mcp.exe` 对同一个扩展实例的偶发断线**保持 AI 端 stdio 连接不掉线**：

- 偶发管道断开 —— 所有等待中的请求立刻以 `disconnected` 失败，下一次请求触发重连

每次 `!mcpext.start` 都会在首帧 `hello` 中生成新的 `bridge_instance_id`。host 会固定
首次看到的 ID；扩展重载、WinDbg 重启，或另一 WinDbg 复用同名 endpoint 时都会被
拒绝。要绑定新实例，需显式重启 `windbg-mcp.exe`。

每次连接都会获得单调递增的 generation。断线后，旧 generation 的排队任务会被
丢弃，其 response/chunk 也不会发送给新 client。扩展在两个 client 之间持续持有
同一个服务端 pipe handle，因此其他进程无法趁重连窗口抢占 endpoint。

## 线路协议

单个全双工命名管道，承载长度前缀的 JSON 帧。

```
+------------------+----------------------------+
|  4 字节          |  N 字节                    |
|  uint32 LE       |  UTF-8 JSON payload        |
|  payload 长度    |                            |
+------------------+----------------------------+
```

6 种帧：`hello` / `req` / `resp` / `ack` / `event` / `chunk`。请求通过 `id` 多路复用；事件异步推送。
不同 WinDbg/host 组合使用唯一 endpoint 时可同时运行。完整规范和错误码列表见
`docs/protocol.md`。

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
    └── host/                     gtest：帧编解码、解析器、endpoint/参数
```

## 状态

v2.0。管道传输、事件推送、核心调试操作和 `.reboot` 生命周期已在真机内核调试
会话（VirtualKD + WinDbg Preview）上验证通过。已知限制：

- 单个 WinDbg 内的请求在一个 dbgeng worker 上 FIFO 串行执行；这是为了遵守
  `IDebugClient` 线程亲和性而做的取舍。长时间 `wm_wait_event` 会推迟同一
  endpoint 上的后续请求；并行能力来自不同 endpoint
- `wm_analyze_crash` 解析器基于 Windows 10/11 的 `!analyze -v` 输出开发；
  老版本可能需要补充解析规则

## License

MIT。见 `LICENSE`。
