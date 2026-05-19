// SPDX-License-Identifier: MIT
//
// Minimal MCP server: speaks JSON-RPC 2.0 over StdioServer. Handles
// MCP's core method set:
//
//   initialize           -> negotiate protocol version, advertise tools cap
//   notifications/*      -> ignore (we don't ever notify; we only respond)
//   tools/list           -> return our six tool descriptors
//   tools/call           -> dispatch to ToolDispatcher
//
// Anything else gets a JSON-RPC method_not_found error.

#pragma once

#include "mcp/stdio_io.h"
#include "tools/dispatcher.h"

#include <nlohmann/json.hpp>

namespace wmh::mcp {

class JsonRpcServer {
public:
    JsonRpcServer(StdioServer& io, tools::Dispatcher& disp);

    void OnLine(const std::string& line);

private:
    nlohmann::json HandleInitialize(const nlohmann::json& params);
    nlohmann::json HandleToolsList();
    nlohmann::json HandleToolsCall(const nlohmann::json& params);

    void SendResult(const nlohmann::json& id, const nlohmann::json& result);
    void SendError (const nlohmann::json& id, int code, const std::string& msg,
                    const nlohmann::json& data = nullptr);

    StdioServer&         m_io;
    tools::Dispatcher&   m_disp;
};

} // namespace wmh::mcp
