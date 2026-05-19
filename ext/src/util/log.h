// SPDX-License-Identifier: MIT
//
// Optional logging on the ext side. Enabled at compile time via the CMake
// option WINDBGMCP_ENABLE_LOGGING. When disabled (the default), WMCP_LOG
// expands to a no-op and no on-disk file is created.
//
// When enabled, output goes to:
//   - OutputDebugStringW (visible to DebugView)
//   - <DLL-directory>/mcpext.log (append mode, share-deny-write)
//
// We do NOT use dprintf because dprintf is only safe to call when WinDbg's
// output callbacks are active, which is not the case from our worker
// threads.

#pragma once

#include <string>
#include <string_view>

namespace windbgmcp::log {

enum class Level {
    Trace,
    Info,
    Warn,
    Error,
};

void Set_min_level(Level lvl);
void Write(Level lvl, std::wstring_view line);
void Write(Level lvl, std::string_view line);

} // namespace windbgmcp::log

#if defined(WINDBGMCP_LOG_ENABLED)
    #define WMCP_LOG(level, msg) ::windbgmcp::log::Write(::windbgmcp::log::Level::level, (msg))
#else
    #define WMCP_LOG(level, msg) ((void)sizeof(msg))
#endif
