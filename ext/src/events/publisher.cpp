// SPDX-License-Identifier: MIT
#include "events/publisher.h"

#include "ipc/pipe_server.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>

namespace windbgmcp::events {

using nlohmann::json;

Publisher& Publisher::Get() {
    static Publisher p;
    return p;
}

bool Publisher::MatchesAny(const Event& e, const std::vector<std::string>& kinds) const {
    return std::any_of(kinds.begin(), kinds.end(),
                       [&](const std::string& k) { return k == e.kind; });
}

void Publisher::Publish(Event ev) {
    // 1. record in history
    Singleton().Push(ev);

    // 2. notify in-process waiters
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_last = ev;
        ++m_seq;
        m_cv.notify_all();
    }

    // 3. push to pipe (if connected)
    if (m_pipe) {
        json frame = {
            {"frame", "event"},
            {"kind",  ev.kind},
            {"ts",    ev.ts_ms},
            {"data",  ev.data},
        };
        m_pipe->Send(frame.dump());
    }
}

std::optional<Event> Publisher::Wait(const std::vector<std::string>& kinds,
                                     std::uint32_t timeout_ms,
                                     std::uint64_t since_ms) {
    // First, history replay if requested.
    if (since_ms > 0) {
        auto past = Singleton().Since(since_ms);
        for (const auto& e : past) {
            if (MatchesAny(e, kinds)) return e;
        }
    }

    // Subscribe to future events.
    std::unique_lock<std::mutex> lk(m_mu);
    std::uint64_t cursor = m_seq;
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);

    while (true) {
        if (m_cv.wait_until(lk, deadline, [&] { return m_seq != cursor; })) {
            // A new event landed; check it then advance the cursor so the
            // next wait_until fires only on the next event.
            cursor = m_seq;
            if (MatchesAny(m_last, kinds)) {
                return m_last;
            }
            continue;
        }
        return std::nullopt; // timeout
    }
}

} // namespace windbgmcp::events
