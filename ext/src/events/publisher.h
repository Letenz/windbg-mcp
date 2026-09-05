// SPDX-License-Identifier: MIT
//
// Single point that turns an Event into a wire frame and pushes it both to
// the history ring and to the connected pipe client. Also notifies any
// in-process waiters (used by handlers that need to wait synchronously,
// e.g. break_in waiting on the next `break` event).

#pragma once

#include "events/event_history.h"

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace windbgmcp::ipc { class PipeServer; }

namespace windbgmcp::events {

class Publisher {
public:
    static Publisher& Get();

    void SetPipe(const std::shared_ptr<ipc::PipeServer>& pipe);
    void ClearPipe();
    void ResumeWaiters();
    void CancelWaiters();
    std::uint64_t DroppedEvents() const noexcept {
        return m_dropped_events.load(std::memory_order_relaxed);
    }

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

    void SenderLoop();

    struct OutboundFrame {
        std::string payload;
        std::uint64_t generation = 0;
    };

    static constexpr std::size_t kOutboundCapacity = 256;

    std::mutex                      m_pipe_mu;
    std::weak_ptr<ipc::PipeServer>  m_pipe;
    std::mutex                      m_out_mu;
    std::condition_variable         m_out_cv;
    std::deque<OutboundFrame>       m_outbound;
    std::thread                     m_sender;
    bool                            m_sender_running = false;
    std::atomic<std::uint64_t>      m_dropped_events{0};
    std::mutex                      m_mu;
    std::condition_variable         m_cv;
    Event                           m_last;       // last published event
    std::uint64_t                   m_seq = 0;    // monotonic seq for waiters
    bool                            m_waits_cancelled = false;
};

} // namespace windbgmcp::events
