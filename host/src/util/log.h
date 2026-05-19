// SPDX-License-Identifier: MIT
//
// Optional logging on the host side. Enabled at compile time via the
// CMake option WINDBGMCP_ENABLE_LOGGING. With logging disabled (the
// default), WMCP_LOG expands to a no-op and no on-disk file is created —
// production deployments don't write to the operator's disk on every
// tool call.
//
// When enabled, output goes to two sinks:
//   - OutputDebugStringA (visible to DebugView)
//   - <exe-directory>/windbg-mcp.log (append mode, share-deny-write)

#pragma once

#include <string>
#include <string_view>

namespace wmh::log {

enum class Level { Trace, Info, Warn, Error };

void SetMinLevel(Level lvl);
void Write(Level lvl, std::string_view line);

} // namespace wmh::log

#if defined(WINDBGMCP_LOG_ENABLED)
    #define WMCP_LOG(level, msg) ::wmh::log::Write(::wmh::log::Level::level, (msg))
#else
    // Compile to (void)0 so callers don't need #ifdef around every call.
    // We still reference (msg) inside the cast so accidental side-effecting
    // arguments do compile (and aren't elided silently).
    #define WMCP_LOG(level, msg) ((void)sizeof(msg))
#endif
