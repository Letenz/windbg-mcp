// SPDX-License-Identifier: MIT
//
// In-process ring buffer of recent events. The publisher pushes; wait_event
// handlers query. Thread-safe.
//
// Eviction: LIFO is preserved on push; oldest are removed when capacity is
// exceeded or when ts is older than kEventRingMaxAgeMs.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace windbgmcp::events {

struct Event {
    std::uint64_t   ts_ms;     // unix epoch ms (UTC)
    std::string     kind;      // "bugcheck" | "break" | ...
    nlohmann::json  data;      // event-specific payload
};

class History {
public:
    History();

    void Push(Event ev);

    // Snapshot of events newer than `since_ms`, oldest first. Excludes any
    // events older than the kMaxAgeMs cutoff at call time.
    std::vector<Event> Since(std::uint64_t since_ms) const;

    // Capacity / cutoff configuration (test seam).
    void SetCapacity(std::size_t n);
    void SetMaxAgeMs(std::uint64_t ms);

private:
    void GcLocked() const;

    mutable std::mutex          m_mu;
    mutable std::vector<Event>  m_buf;       // ring (vector + head idx)
    std::size_t                 m_cap;
    std::uint64_t               m_max_age_ms;
};

// Process-wide singleton accessor.
History& Singleton();

// Wall-clock helper.
std::uint64_t NowMs();

} // namespace windbgmcp::events
