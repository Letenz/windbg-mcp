// SPDX-License-Identifier: MIT
//
// Monotonic connection generations for fencing work produced by a previous
// named-pipe client. A generation is active only between Open() and Close().

#pragma once

#include <atomic>
#include <cstdint>

namespace windbgmcp::ipc {

using ConnectionGeneration = std::uint64_t;
inline constexpr ConnectionGeneration kNoConnectionGeneration = 0;

class ConnectionFence {
public:
    ConnectionGeneration Open() noexcept {
        const auto generation = m_next.fetch_add(1, std::memory_order_relaxed) + 1;
        m_active.store(generation, std::memory_order_release);
        return generation;
    }

    void Close(ConnectionGeneration generation) noexcept {
        auto expected = generation;
        m_active.compare_exchange_strong(
            expected, kNoConnectionGeneration,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    ConnectionGeneration Current() const noexcept {
        return m_active.load(std::memory_order_acquire);
    }

    bool Allows(ConnectionGeneration generation) const noexcept {
        return generation != kNoConnectionGeneration && Current() == generation;
    }

private:
    std::atomic<ConnectionGeneration> m_next{kNoConnectionGeneration};
    std::atomic<ConnectionGeneration> m_active{kNoConnectionGeneration};
};

} // namespace windbgmcp::ipc
