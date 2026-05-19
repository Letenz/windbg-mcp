// SPDX-License-Identifier: MIT
//
// Single point that turns an Event into a wire frame and pushes it both to
// the history ring and to the connected pipe client. Also notifies any
// in-process waiters (used by handlers that need to wait synchronously,
// e.g. break_in waiting on the next `break` event).

#pragma once

#include "events/event_history.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace windbgmcp::ipc { class PipeServer; }

namespace windbgmcp::events {

class Publisher {
public:
    static Publisher& Get();

    void SetPipe(ipc::PipeServer* pipe) { m_pipe = pipe; }

    // Push a fully-formed event. Thread-safe; can be called from dbgeng
    // callbacks.
    void Publish(Event ev);

    // Block the caller until an event whose `kind` is in `kinds` arrives,
    // or `timeout_ms` elapses. If `since_ms > 0`, the history ring is
    // checked first. Returns nullopt on timeout.
    std::optional<Event> Wait(const std::vector<std::string>& kinds,
                              std::uint32_t timeout_ms,
                              std::uint64_t since_ms);

private:
    Publisher() = default;

    bool MatchesAny(const Event& e, const std::vector<std::string>& kinds) const;

    ipc::PipeServer*                m_pipe = nullptr;
    std::mutex                      m_mu;
    std::condition_variable         m_cv;
    Event                           m_last;       // last published event
    std::uint64_t                   m_seq = 0;    // monotonic seq for waiters
};

} // namespace windbgmcp::events
