// SPDX-License-Identifier: MIT
//
// windbg-mcp.exe entry point.
//
// Lifecycle:
//   1. Start the StdioServer (reads JSON-RPC lines from stdin).
//   2. Lazily-connect the PipeClient on the first tool call.
//   3. Process MCP requests synchronously; tools/call dispatches to
//      Dispatcher which talks to the pipe.
//   4. When stdin closes, exit.

#include "mcp/jsonrpc_server.h"
#include "mcp/stdio_io.h"
#include "tools/dispatcher.h"
#include "transport/pipe_client.h"
#include "util/log.h"

int main(int argc, char* argv[]) {
#if defined(WINDBGMCP_LOG_ENABLED)
    // Logging is compiled in. Default level is Info; env WINDBGMCP_LOG=1
    // bumps to Trace for transport-level diagnosis.
    if (auto* v = std::getenv("WINDBGMCP_LOG"); v && *v == '1') {
        wmh::log::SetMinLevel(wmh::log::Level::Trace);
    }
#endif

    (void)argc; (void)argv;

    wmh::transport::PipeClient pipe;
    wmh::tools::Dispatcher     disp(pipe);
    wmh::mcp::StdioServer      io;
    wmh::mcp::JsonRpcServer    rpc(io, disp);

    io.Start([&rpc](const std::string& line) { rpc.OnLine(line); });
    io.Join();
    pipe.Close();
    return 0;
}
