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

#include "app/options.h"
#include "mcp/jsonrpc_server.h"
#include "mcp/stdio_io.h"
#include "tools/dispatcher.h"
#include "transport/pipe_client.h"
#include "util/log.h"

#include "windbgmcp/protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
#if defined(WINDBGMCP_LOG_ENABLED)
    // Logging is compiled in. Default level is Info; env WINDBGMCP_LOG=1
    // bumps to Trace for transport-level diagnosis.
    if (auto* v = std::getenv("WINDBGMCP_LOG"); v && *v == '1') {
        wmh::log::SetMinLevel(wmh::log::Level::Trace);
    }
#endif

    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    char* raw_env_pipe = nullptr;
    std::size_t raw_env_size = 0;
    const errno_t env_result = ::_dupenv_s(
        &raw_env_pipe, &raw_env_size, windbgmcp::kPipeEnvironmentVariable);
    const std::string env_pipe =
        env_result == 0 && raw_env_pipe ? raw_env_pipe : std::string{};
    std::free(raw_env_pipe);

    const auto options = wmh::app::ParseOptions(
        args, env_pipe.empty() ? nullptr : env_pipe.c_str());
    if (!options.ok) {
        std::cerr << "windbg-mcp: " << options.error << "\n"
                  << wmh::app::UsageText();
        return 2;
    }
    if (options.show_help) {
        std::cout << wmh::app::UsageText();
        return 0;
    }

    wmh::transport::PipeClient pipe(options.options.pipe_endpoint);
    wmh::tools::Dispatcher     disp(pipe);
    wmh::mcp::StdioServer      io;
    wmh::mcp::JsonRpcServer    rpc(io, disp);

    io.Start([&rpc](const std::string& line) { rpc.OnLine(line); });
    io.Join();
    pipe.Close();
    return 0;
}
