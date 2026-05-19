// SPDX-License-Identifier: MIT
//
// ANSI <-> UTF-8 helpers. WinDbg's output callbacks deliver bytes in the
// console codepage (almost always CP_ACP on a Western locale install); the
// MCP wire is UTF-8. We do the conversion in one place to keep handlers
// simple.

#pragma once

#include <string>
#include <string_view>

namespace windbgmcp::encoding {

// Convert a byte string in CP_ACP (current process ANSI codepage) to UTF-8.
// Returns an empty string on conversion failure rather than throwing — the
// callers all just want best-effort text and propagate the empty result.
std::string AcpToUtf8(std::string_view acp);

// Convert UTF-8 to wide. Used for opening files / showing in WinDbg UI.
std::wstring Utf8ToWide(std::string_view utf8);

// Inverse of the above; rarely needed but used by exports.cpp to emit help
// strings via dprintf (which is ANSI).
std::string WideToUtf8(std::wstring_view wide);
std::string WideToAcp(std::wstring_view wide);

} // namespace windbgmcp::encoding
