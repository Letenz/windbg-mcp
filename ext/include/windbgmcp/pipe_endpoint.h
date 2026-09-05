// SPDX-License-Identifier: MIT
//
// Shared named-pipe endpoint parsing for mcpext.dll and windbg-mcp.exe.
// Keeping this header-only lets both binaries (and unit tests) use exactly
// the same validation without introducing another common library target.

#pragma once

#include "windbgmcp/protocol.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace windbgmcp {

struct PipeEndpointResult {
    bool        ok = false;
    std::string endpoint;
    std::string error;
};

namespace detail {

inline bool IsAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline char AsciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool StartsWithInsensitive(std::string_view value,
                                  std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (AsciiLower(value[i]) != AsciiLower(prefix[i])) return false;
    }
    return true;
}

inline bool IsPipeNameChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.';
}

} // namespace detail

// Accept either a short pipe name ("windbgmcp-run-42") or the canonical
// local endpoint ("\\\\.\\pipe\\windbgmcp-run-42"). The deliberately small
// character set keeps WinDbg bang-command and MCP config quoting predictable.
// Empty input selects the default only when allow_default is true.
inline PipeEndpointResult NormalizePipeEndpoint(std::string_view raw,
                                                bool allow_default = true) {
    while (!raw.empty() && detail::IsAsciiSpace(raw.front())) raw.remove_prefix(1);
    while (!raw.empty() && detail::IsAsciiSpace(raw.back())) raw.remove_suffix(1);

    if (raw.empty()) {
        if (allow_default) return {true, kDefaultPipeEndpoint, {}};
        return {false, {}, "pipe endpoint must not be empty"};
    }

    // WinDbg passes !command arguments as one raw string. Accept a matching
    // quote pair so generated command lines can quote the endpoint safely.
    if (raw.front() == '"' || raw.back() == '"') {
        if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
            return {false, {}, "pipe endpoint has an unmatched quote"};
        }
        raw.remove_prefix(1);
        raw.remove_suffix(1);
        if (raw.empty()) return {false, {}, "pipe endpoint must not be empty"};
    }

    constexpr std::size_t kMaxPipeNameBytes = 240;
    std::string_view name = raw;
    if (detail::StartsWithInsensitive(raw, kPipeEndpointPrefix)) {
        name.remove_prefix(std::string_view(kPipeEndpointPrefix).size());
    } else if (raw.find('\\') != std::string_view::npos ||
               raw.find('/') != std::string_view::npos) {
        return {false, {},
                "pipe endpoint must be a short name or start with \\\\.\\pipe\\"};
    }

    if (name.empty()) return {false, {}, "pipe name must not be empty"};
    if (name.size() > kMaxPipeNameBytes) {
        return {false, {}, "pipe name is too long (maximum 240 characters)"};
    }
    if (name == "." || name == "..") {
        return {false, {}, "pipe name must not be '.' or '..'"};
    }
    for (char c : name) {
        if (!detail::IsPipeNameChar(c)) {
            return {false, {},
                    "pipe name may contain only ASCII letters, digits, '.', '_' and '-'"};
        }
    }

    return {true, std::string(kPipeEndpointPrefix) + std::string(name), {}};
}

inline std::wstring PipeEndpointToWide(std::string_view endpoint) {
    // NormalizePipeEndpoint guarantees ASCII, so a direct widening is exact.
    return std::wstring(endpoint.begin(), endpoint.end());
}

} // namespace windbgmcp
