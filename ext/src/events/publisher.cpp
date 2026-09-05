// SPDX-License-Identifier: MIT
#include "events/publisher.h"

#include "ipc/pipe_server.h"
#include "lifecycle/unload_barrier.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>

namespace windbgmcp::events {

using nlohmann::json;

Publisher& Publisher::Get() {
    // Process-lifetime storage avoids std::thread static destruction during
    // DLL detach. DebugExtensionCanUnload still requires the sender activity
    // counter to be zero before the image may unload.
    static Publisher* publisher = new Publisher();
    return *publisher;
}

void Publisher::SetPipe(const std::shared_ptr<ipc::PipeServer>& pipe) {
    ClearPipe();
    {
        std::lock_guard<std::mutex> lk(m_pipe_mu);
        m_pipe = pipe;
    }
    {
        std::lock_guard<std::mutex> lk(m_out_mu);
        m_outbound.clear();
        m_sender_running = true;
        m_dropped_events.store(0, std::memory_order_relaxed);
    }
    try {
        m_sender = std::thread([this] { SenderLoop(); });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(m_out_mu);
            m_sender_running = false;
        }
        std::lock_guard<std::mutex> lk(m_pipe_mu);
        m_pipe.reset();
        WMCP_LOG(Error,
                 "failed to start event sender; async events will be dropped");
    }
}

void Publisher::ClearPipe() {
    {
        std::lock_guard<std::mutex> lk(m_out_mu);
        m_sender_running = false;
        m_outbound.clear();
    }
    m_out_cv.notify_all();
    if (m_sender.joinable()) {
        if (m_sender.get_id() == std::this_thread::get_id()) {
            WMCP_LOG(Error,
                     "Publisher::ClearPipe refused sender self-join");
        } else {
            m_sender.join();
        }
    }
    std::lock_guard<std::mutex> lk(m_pipe_mu);
    m_pipe.reset();
}

void Publisher::ResumeWaiters() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_waits_cancelled = false;
}

void Publisher::CancelWaiters() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_waits_cancelled = true;
    m_cv.notify_all();
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

    // 3. enqueue for the dedicated sender. dbgeng invokes Publish from its
    // callback thread; that thread must never wait on named-pipe I/O. Drop the
    // newest event under backpressure, while retaining history/wait semantics.
    std::string payload;
    try {
        payload = json{
            {"frame", "event"},
            {"kind",  ev.kind},
            {"ts",    ev.ts_ms},
            {"data",  ev.data},
        }.dump();
    } catch (...) {
        m_dropped_events.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::shared_ptr<ipc::PipeServer> target_pipe;
    {
        std::lock_guard<std::mutex> lk(m_pipe_mu);
        target_pipe = m_pipe.lock();
    }
    if (!target_pipe) return;
    const auto generation = target_pipe->CurrentGeneration();
    if (generation == ipc::kNoConnectionGeneration) return;

    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(m_out_mu);
        if (m_sender_running && m_outbound.size() < kOutboundCapacity) {
            try {
                m_outbound.push_back(OutboundFrame{
                    std::move(payload), generation});
                queued = true;
            } catch (...) {
                m_dropped_events.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (m_sender_running) {
            m_dropped_events.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (queued) m_out_cv.notify_one();
}

void Publisher::SenderLoop() {
    lifecycle::ActivityGuard activity(lifecycle::ActivityKind::PublisherWorker);
    while (true) {
        OutboundFrame frame;
        {
            std::unique_lock<std::mutex> lk(m_out_mu);
            m_out_cv.wait(lk, [this] {
                return !m_sender_running || !m_outbound.empty();
            });
            if (!m_sender_running) break;
            frame = std::move(m_outbound.front());
            m_outbound.pop_front();
        }

        std::shared_ptr<ipc::PipeServer> pipe;
        {
            std::lock_guard<std::mutex> lk(m_pipe_mu);
            pipe = m_pipe.lock();
        }
        if (pipe && !pipe->Send(frame.payload, frame.generation)) {
            WMCP_LOG(Warn, "async event send failed or timed out");
        }
    }
}

std::optional<Event> Publisher::Wait(const std::vector<std::string>& kinds,
                                     std::uint32_t timeout_ms,
                                     std::uint64_t since_ms) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_waits_cancelled) return std::nullopt;
    }

    // First, history replay if requested.
    if (since_ms > 0) {
        auto past = Singleton().Since(since_ms);
        for (const auto& e : past) {
            if (MatchesAny(e, kinds)) return e;
        }
    }

    // Subscribe to future events.
    std::unique_lock<std::mutex> lk(m_mu);
    if (m_waits_cancelled) return std::nullopt;
    std::uint64_t cursor = m_seq;
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);

    while (true) {
        if (m_cv.wait_until(lk, deadline, [&] {
                return m_waits_cancelled || m_seq != cursor;
            })) {
            if (m_waits_cancelled) return std::nullopt;
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
