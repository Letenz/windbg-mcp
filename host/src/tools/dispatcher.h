// SPDX-License-Identifier: MIT
//
// Tool dispatcher. Owns the PipeClient and the 6 MCP tools. Each tool
// translates MCP `tools/call` arguments into one or more pipe requests,
// then formats the result as a UTF-8 string that the MCP client receives.
//
// MCP convention: a tool returns a "content" array of {type, text} items.
// We always emit a single text item, where text is a compact JSON dump of
// the structured result. The AI happily parses JSON-in-text and this
// avoids us having to invent a per-tool schema in tools/list.

#pragma once

#include "transport/pipe_client.h"

#include <nlohmann/json.hpp>
#include <string>

namespace wmh::tools {

struct Result {
    std::string text;
    bool        is_error = false;
};

class Dispatcher {
public:
    explicit Dispatcher(transport::PipeClient& pipe);

    // Returns the list of tool descriptors for tools/list.
    nlohmann::json Descriptors() const;

    // Invoke a tool by MCP name (e.g. "wm_session"). Always returns a
    // Result; failures are encoded with is_error=true and a structured
    // error text.
    Result Call(const std::string& name, const nlohmann::json& args);

private:
    Result CallSession    (const nlohmann::json& args);
    Result CallRunCmd     (const nlohmann::json& args);
    Result CallWaitEvent  (const nlohmann::json& args);
    Result CallBreakIn    (const nlohmann::json& args);
    Result CallAnalyzeCrash(const nlohmann::json& args);
    Result CallExit       (const nlohmann::json& args);

    transport::PipeClient& m_pipe;
};

// Helpers shared with individual tool .cpp files.
Result OkJson  (const nlohmann::json& body);
Result ErrJson (const std::string& code, const std::string& msg,
                const std::string& tip = "", long hr = 0);
Result FromResponse(const transport::Response& r);

} // namespace wmh::tools
