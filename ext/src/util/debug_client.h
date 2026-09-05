// SPDX-License-Identifier: MIT
//
// Owns the request lane's IDebugClient. The router executes every dbgeng
// handler on one stable worker thread; Get() creates the client there and
// refuses to hand the same COM object to any other thread. WinDbg event
// callbacks use the client passed to !mcpext.start instead.
//
// Lifetime: created on first request-lane call to Get(), then released by
// Reset() on that same worker when the router stops.
//
// dbgeng serialisation: dbgeng's kernel-debug transport (KD packets) can
// only be used by one caller at a time. Concurrent KD-touching calls
// produce "Kernel transport in use, packet write failed". Handlers that
// invoke any IDebugControl method that may issue a KD packet (Execute,
// ReadBugCheckData, GetNumberModules on a fresh session, GetModuleByOffset,
// etc.) must lock the engine via dbg::Lock first.

#pragma once

#include <DbgEng.h>
#include <atlbase.h>
#include <mutex>

namespace windbgmcp::dbg {

// Returns the request lane's lazily-created IDebugClient. Returns nullptr if
// DebugCreate failed or a caller violates the lane's thread affinity.
CComPtr<IDebugClient> Get();

// Drop the cached client. Must be called from its owning request thread.
void Reset();

// Process-wide mutex serialising all dbgeng/KD-touching calls. Take it
// via std::lock_guard<std::mutex>(dbg::Lock()).
std::mutex& Lock();

} // namespace windbgmcp::dbg
