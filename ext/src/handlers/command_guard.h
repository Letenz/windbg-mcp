// SPDX-License-Identifier: MIT

#pragma once

#include "handlers/command_batch.h"

namespace windbgmcp::handlers {

// Commands that can synchronously stop/unload this extension must never run
// on the Router worker: dbgeng can invoke DebugExtensionUninitialize before
// Execute returns, making safe self-join and owner-thread callback cleanup
// impossible.
inline std::optional<std::string> UnsafeLifecycleCommand(std::string_view command) {
    for (const auto& token : StatementTokens(command)) {
        if (token.starts_with("!mcpext")) return "use the dedicated bridge lifecycle tools";
        if (token.starts_with(".unload") || token == ".reboot" || token == ".restart")
            return "this command can unload mcpext synchronously";
        if (token == "q" || token == "qq" || token == "qd") {
            return "debugger quit commands can unload mcpext synchronously";
        }
    }
    return std::nullopt;
}

} // namespace windbgmcp::handlers
