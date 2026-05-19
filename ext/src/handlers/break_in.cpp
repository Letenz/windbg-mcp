// SPDX-License-Identifier: MIT
//
// break_in: SetInterrupt + wait on the publisher for the next `break`
// event. No polling.
//
// Implementation note: SetInterrupt is broadcast across all dbgeng clients
// in the engine, so we can issue it from a freshly-created IDebugClient
// without having to be the engine's owning client. We then block on
// Publisher::Wait, which is fed by our event sink running on whichever
// client owns the engine loop (WinDbg's UI client, normally).

#include "handlers/handlers.h"

#include "events/publisher.h"
#include "util/debug_client.h"
#include "util/log.h"
#include "util/status.h"
#include "windbgmcp/protocol.h"

#include <DbgEng.h>
#include <atlbase.h>

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

json BreakIn(std::int64_t /*req_id*/, const json& args, ipc::PipeServer& /*pipe*/) {
    const std::uint32_t timeout_ms =
        args.value("timeout_ms", static_cast<std::uint32_t>(kTimeoutStandardMs));

    auto client = dbg::Get();
    if (!client) {
        throw HandlerError(err::kEngineError, "no IDebugClient", "");
    }
    CComQIPtr<IDebugControl4> ctl(client);
    if (!ctl) {
        throw HandlerError(err::kEngineError, "QI IDebugControl4 failed", "");
    }

    // We hold the dbgeng lock only while reading status and issuing
    // SetInterrupt. The subsequent wait on the publisher must NOT hold the
    // lock — if it did, the event sink could not advance state and we'd
    // deadlock waiting for an event that can never fire.
    ULONG before = 0;
    {
        std::lock_guard<std::mutex> engine_lk(dbg::Lock());
        ctl->GetExecutionStatus(&before);
    }
    if (!status::IsRunning(before)) {
        // Already broken; nothing to do.
        return json{
            {"ok",             true},
            {"already_broken", true},
            {"prior_status",   status::ToString(before)},
            {"current_status", status::ToString(before)},
        };
    }

    // Issue the interrupt request from a fresh client to avoid touching
    // the engine's owning client.
    CComPtr<IDebugClient> req_client;
    HRESULT hr = ::DebugCreate(__uuidof(IDebugClient), reinterpret_cast<void**>(&req_client));
    if (FAILED(hr)) {
        throw HandlerError(err::kEngineError, "DebugCreate (interrupt) failed", "", hr);
    }
    CComQIPtr<IDebugControl> req_ctl(req_client);
    if (!req_ctl) {
        throw HandlerError(err::kEngineError, "QI IDebugControl (interrupt) failed", "");
    }
    {
        std::lock_guard<std::mutex> engine_lk(dbg::Lock());
        hr = req_ctl->SetInterrupt(DEBUG_INTERRUPT_ACTIVE);
    }
    if (FAILED(hr)) {
        throw HandlerError(err::kEngineError, "SetInterrupt failed", "", hr);
    }

    // Wait for the next `break` (or `bugcheck`, in case the target was
    // about to bugcheck anyway).  No dbgeng calls held during this wait.
    auto ev = events::Publisher::Get().Wait(
        {"break", "bugcheck"}, timeout_ms, /*since_ms=*/0);

    ULONG after = 0;
    {
        std::lock_guard<std::mutex> engine_lk(dbg::Lock());
        ctl->GetExecutionStatus(&after);
    }

    if (!ev) {
        throw HandlerError(err::kTimeout,
                           "interrupt sent but no break event arrived in time",
                           "raise timeout_ms or check if guest is hung");
    }

    return json{
        {"ok",             true},
        {"already_broken", false},
        {"prior_status",   status::ToString(before)},
        {"current_status", status::ToString(after)},
        {"break_event",    ev->kind},
    };
}

} // namespace windbgmcp::handlers
