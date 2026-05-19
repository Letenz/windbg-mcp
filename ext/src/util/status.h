// SPDX-License-Identifier: MIT
//
// dbgeng status helpers: ULONG -> string mapping (mirror of docs/events.md)
// and a small predicate helper used by handlers.

#pragma once

#include <DbgEng.h>
#include <string>

namespace windbgmcp::status {

// Map a DEBUG_STATUS_* ULONG to its public text form ("GO", "BREAK", ...).
// Returns "UNKNOWN" for values outside the documented set.
const char* ToString(ULONG dbgeng_status);

// Returns true iff the given dbgeng status indicates the target is currently
// executing (any GO/STEP variant, including reverse).
bool IsRunning(ULONG dbgeng_status);

// Returns true iff the status is BREAK.
bool IsBroken(ULONG dbgeng_status);

} // namespace windbgmcp::status
