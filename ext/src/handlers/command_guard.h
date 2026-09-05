// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace windbgmcp::handlers {

inline std::string AsciiLowerCopy(std::string_view value) {
    std::string out(value);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

inline std::string_view TrimAscii(std::string_view value) noexcept {
    auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    return value;
}

// Commands that can synchronously stop/unload this extension must never run
// on the Router worker: dbgeng can invoke DebugExtensionUninitialize before
// Execute returns, making safe self-join and owner-thread callback cleanup
// impossible.
inline std::optional<std::string> UnsafeLifecycleCommand(std::string_view command) {
    const std::string lower = AsciiLowerCopy(command);
    if (lower.find("!mcpext") != std::string::npos) {
        return "mcpext lifecycle commands must run in the WinDbg command window";
    }
    if (lower.find(".reboot") != std::string::npos) {
        return ".reboot can unload mcpext synchronously";
    }
    if (lower.find(".unload") != std::string::npos) {
        return ".unload can unload mcpext synchronously";
    }

    std::size_t begin = 0;
    while (begin <= lower.size()) {
        const auto end = lower.find_first_of(";\r\n", begin);
        const auto segment = TrimAscii(std::string_view(lower).substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        const auto token_end = segment.find_first_of(" \t");
        const auto first_token = segment.substr(0, token_end);
        if (first_token == "q" || first_token == "qq" || first_token == "qd") {
            return "debugger quit commands can unload mcpext synchronously";
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return std::nullopt;
}

} // namespace windbgmcp::handlers
