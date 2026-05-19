// SPDX-License-Identifier: MIT
//
// Owns the process-wide IDebugClient that the extension uses for everything
// that runs OUTSIDE a WinDbg-issued callback (event sink registration,
// session queries, break_in interrupts). Handlers that *are* inside a
// WinDbg callback (the way `mcpext_start` is called) get the client passed
// in directly by WinDbg.
//
// Lifetime: created on first call to Get(), released by Reset().
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

// Returns a process-wide IDebugClient. Lazily created. Thread-safe.
// May return nullptr if DebugCreate failed (extremely rare).
CComPtr<IDebugClient> Get();

// Drop the cached client. Call from extension teardown only.
void Reset();

// Process-wide mutex serialising all dbgeng/KD-touching calls. Take it
// via std::lock_guard<std::mutex>(dbg::Lock()).
std::mutex& Lock();

} // namespace windbgmcp::dbg
