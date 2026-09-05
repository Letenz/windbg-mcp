// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace windbgmcp::lifecycle {

enum class ActivityKind : std::size_t {
    RouterWorker = 0,
    CallbackActor = 1,
    Teardown = 2,
    PublisherWorker = 3,
};

struct ActivitySnapshot {
    std::uint32_t router_workers = 0;
    std::uint32_t callback_actors = 0;
    std::uint32_t teardowns = 0;
    std::uint32_t publisher_workers = 0;

    bool Quiescent() const noexcept {
        return router_workers == 0 && callback_actors == 0 && teardowns == 0 &&
               publisher_workers == 0;
    }
};

class UnloadBarrier {
public:
    static UnloadBarrier& Get() {
        // Process-lifetime storage avoids a static-destruction race during
        // extension unload. All counters must be zero before unload anyway.
        static UnloadBarrier* barrier = new UnloadBarrier();
        return *barrier;
    }

    void Enter(ActivityKind kind) noexcept {
        m_counts[Index(kind)].fetch_add(1, std::memory_order_acq_rel);
    }

    void Leave(ActivityKind kind) noexcept {
        m_counts[Index(kind)].fetch_sub(1, std::memory_order_acq_rel);
    }

    ActivitySnapshot Snapshot() const noexcept {
        return ActivitySnapshot{
            m_counts[Index(ActivityKind::RouterWorker)].load(std::memory_order_acquire),
            m_counts[Index(ActivityKind::CallbackActor)].load(std::memory_order_acquire),
            m_counts[Index(ActivityKind::Teardown)].load(std::memory_order_acquire),
            m_counts[Index(ActivityKind::PublisherWorker)].load(std::memory_order_acquire),
        };
    }

private:
    static constexpr std::size_t Index(ActivityKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    std::array<std::atomic<std::uint32_t>, 4> m_counts{};
};

class ActivityGuard {
public:
    explicit ActivityGuard(ActivityKind kind) noexcept : m_kind(kind) {
        UnloadBarrier::Get().Enter(m_kind);
    }
    ~ActivityGuard() { UnloadBarrier::Get().Leave(m_kind); }

    ActivityGuard(const ActivityGuard&) = delete;
    ActivityGuard& operator=(const ActivityGuard&) = delete;

private:
    ActivityKind m_kind;
};

} // namespace windbgmcp::lifecycle
