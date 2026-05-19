// SPDX-License-Identifier: MIT
#include "events/event_history.h"

#include "windbgmcp/protocol.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>

namespace windbgmcp::events {

namespace {
History& Instance() {
    static History h;
    return h;
}
} // namespace

History::History()
    : m_cap(kEventRingCapacity),
      m_max_age_ms(kEventRingMaxAgeMs) {
    m_buf.reserve(m_cap);
}

void History::Push(Event ev) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_buf.push_back(std::move(ev));
    GcLocked();
}

std::vector<Event> History::Since(std::uint64_t since_ms) const {
    std::lock_guard<std::mutex> lk(m_mu);
    GcLocked();
    std::vector<Event> out;
    out.reserve(m_buf.size());
    for (const auto& e : m_buf) {
        if (e.ts_ms >= since_ms) out.push_back(e);
    }
    return out;
}

void History::SetCapacity(std::size_t n) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_cap = n;
    GcLocked();
}

void History::SetMaxAgeMs(std::uint64_t ms) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_max_age_ms = ms;
    GcLocked();
}

void History::GcLocked() const {
    auto& buf = const_cast<std::vector<Event>&>(m_buf);
    // Drop by capacity (keep the newest).
    if (buf.size() > m_cap) {
        buf.erase(buf.begin(), buf.begin() + (buf.size() - m_cap));
    }
    // Drop by age.
    const std::uint64_t now = NowMs();
    if (now > m_max_age_ms) {
        const std::uint64_t cutoff = now - m_max_age_ms;
        auto it = std::find_if(buf.begin(), buf.end(),
                               [cutoff](const Event& e) { return e.ts_ms >= cutoff; });
        if (it != buf.begin()) {
            buf.erase(buf.begin(), it);
        }
    }
}

History& Singleton() {
    return Instance();
}

std::uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace windbgmcp::events
