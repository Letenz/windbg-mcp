// SPDX-License-Identifier: MIT
#include "ipc/router.h"

#include "ipc/pipe_server.h"
#include "lifecycle/unload_barrier.h"
#include "util/debug_client.h"
#include "util/log.h"
#include "windbgmcp/protocol.h"

#include <exception>

namespace windbgmcp::ipc {

using nlohmann::json;

Router::Router() = default;

Router::~Router() {
    // Deleting a Router on its own worker would destroy a joinable thread
    // object and leave WorkerLoop executing through freed memory. Lifecycle
    // code must defer destruction to the owner thread.
    if (!Stop()) std::terminate();
}

void Router::Register(const std::string& op, Handler h) {
    m_handlers[op] = std::move(h);
}

void Router::RegisterFinalResponseAction(const std::string& op,
                                         FinalResponseAction action) {
    m_final_actions[op] = std::move(action);
}

void Router::Start(std::size_t worker_count) {
    if (m_running.exchange(true)) return;
    m_accepting.store(false, std::memory_order_release);
    if (worker_count != 1) {
        WMCP_LOG(Warn, "Router::Start forcing one serial dbgeng worker");
    }
    worker_count = 1;
    m_workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        m_workers.emplace_back([this] { WorkerLoop(); });
    }
}

bool Router::Stop() {
    m_accepting.store(false, std::memory_order_release);
    m_running.store(false);
    m_cv.notify_all();

    if (IsWorkerThread()) {
        WMCP_LOG(Error, "Router::Stop refused self-join; teardown must be deferred");
        return false;
    }

    for (auto& t : m_workers) {
        if (!t.joinable()) continue;
        t.join();
    }
    m_workers.clear();
    std::queue<QueuedPayload> empty;
    std::lock_guard<std::mutex> lk(m_mu);
    std::swap(m_queue, empty);
    return true;
}

bool Router::IsWorkerThread() const noexcept {
    const auto self_id = std::this_thread::get_id();
    for (const auto& t : m_workers) {
        if (t.joinable() && t.get_id() == self_id) return true;
    }
    return false;
}

void Router::OnPayload(std::string payload, ConnectionGeneration generation) {
    WMCP_LOG(Info, std::string("Router::OnPayload: enqueue bytes=") +
                   std::to_string(payload.size()));
    bool queue_full = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_running.load()) return;
        if (m_queue.size() >= kQueueCapacity) {
            queue_full = true;
        } else {
            m_queue.push(QueuedPayload{
                std::move(payload), generation,
                m_accepting.load(std::memory_order_acquire)});
        }
    }
    if (queue_full) {
        RejectQueueFull(std::move(payload), generation);
        return;
    }
    m_cv.notify_one();
}

void Router::WorkerLoop() {
    lifecycle::ActivityGuard activity(lifecycle::ActivityKind::RouterWorker);
    while (m_running.load()) {
        QueuedPayload job;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] { return !m_queue.empty() || !m_running.load(); });
            if (!m_running.load()) break;
            job = std::move(m_queue.front());
            m_queue.pop();
        }

        // A replacement host may reuse request ids. Never execute queued
        // work from a disconnected generation after the new client arrives.
        if (!m_pipe || !m_pipe->IsCurrentGeneration(job.generation)) {
            WMCP_LOG(Warn, "WorkerLoop: dropping stale connection job");
            continue;
        }
        WMCP_LOG(Info, std::string("WorkerLoop: dispatching, bytes=") +
                       std::to_string(job.payload.size()));
        if (job.accepted_after_commit) {
            Dispatch(std::move(job.payload), job.generation);
        } else {
            RejectBeforeCommit(std::move(job.payload), job.generation);
        }
        WMCP_LOG(Info, "WorkerLoop: dispatch returned");
    }
    // Release the request lane's thread-affine IDebugClient on its owner
    // thread, never from WinDbg's extension command/callback thread.
    dbg::Reset();
}

void Router::RejectBeforeCommit(
    std::string payload,
    ConnectionGeneration generation) {
    std::int64_t req_id = 0;
    try {
        const auto msg = json::parse(payload);
        if (msg.is_object() && msg.value("frame", "") == "req") {
            req_id = msg.value("id", static_cast<std::int64_t>(0));
        }
    } catch (...) {
        // A startup-gated payload never reaches a handler. id=0 is the
        // protocol's synthetic correlation for malformed frames.
    }
    SendError(req_id, err::kDisconnected,
              "bridge startup has not committed",
              "retry after !mcpext.start reports listening", 0, generation);
}

void Router::RejectQueueFull(
    std::string payload,
    ConnectionGeneration generation) {
    std::int64_t req_id = 0;
    try {
        const auto msg = json::parse(payload);
        if (msg.is_object() && msg.value("frame", "") == "req") {
            req_id = msg.value("id", static_cast<std::int64_t>(0));
        }
    } catch (...) {
    }
    SendError(req_id, err::kEngineError,
              "request queue capacity exceeded",
              "wait for pending debugger work before retrying", 0, generation);
}

void Router::Dispatch(std::string payload, ConnectionGeneration generation) {
    json msg;
    std::int64_t req_id = 0;
    try {
        msg = json::parse(payload);
    } catch (const std::exception& e) {
        WMCP_LOG(Warn, std::string("invalid JSON frame: ") + e.what());
        // No id available — send a synthetic error with id 0.
        SendError(0, err::kProtocolError, "invalid JSON in frame", "fix client", 0,
                  generation);
        return;
    }

    if (!msg.is_object() || msg.value("frame", "") != "req") {
        SendError(0, err::kProtocolError, "expected frame=req", "", 0, generation);
        return;
    }
    req_id = msg.value("id", static_cast<std::int64_t>(0));
    const std::string op = msg.value("op", std::string{});
    const json args = msg.contains("args") && msg["args"].is_object()
                          ? msg["args"]
                          : json::object();

    auto it = m_handlers.find(op);
    if (it == m_handlers.end()) {
        SendError(req_id, err::kInvalidArg, "unknown op: " + op,
                  "see docs/protocol.md", 0, generation);
        return;
    }

    WMCP_LOG(Info, std::string("Dispatch: calling handler op=") + op +
                   " id=" + std::to_string(req_id));
    try {
        json data = it->second(req_id, args, *m_pipe, generation);
        WMCP_LOG(Info, std::string("Dispatch: handler returned op=") + op +
                       " id=" + std::to_string(req_id));
        const auto final_it = m_final_actions.find(op);
        const bool final_response = final_it != m_final_actions.end();
        const bool sent = SendOk(req_id, data, generation, final_response);
        if (final_response) {
            if (!sent) {
                WMCP_LOG(Warn,
                         "Dispatch: final response was not delivered; "
                         "bridge shutdown suppressed");
            } else {
                // SendFinal has already fenced the connection and stopped
                // the pipe's reader. The callback must only enqueue async
                // teardown because this is the Router worker itself.
                try {
                    final_it->second(generation);
                } catch (const std::exception& e) {
                    WMCP_LOG(Error,
                             std::string("Dispatch: final response action threw: ") +
                                 e.what());
                } catch (...) {
                    WMCP_LOG(Error,
                             "Dispatch: final response action threw an unknown exception");
                }
            }
        }
    } catch (const HandlerError& e) {
        WMCP_LOG(Warn, std::string("Dispatch: handler error op=") + op +
                       " code=" + e.code());
        SendError(req_id, e.code(), e.msg(), e.tip(), e.hr(), generation);
    } catch (const std::exception& e) {
        WMCP_LOG(Error, std::string("Dispatch: handler threw op=") + op +
                        " what=" + e.what());
        SendError(req_id, err::kEngineError,
                  std::string("handler threw: ") + e.what(),
                  "check ext logs", 0, generation);
    }
}

void Router::SendError(std::int64_t req_id, const std::string& code,
                       const std::string& msg, const std::string& tip,
                       long hr, ConnectionGeneration generation) {
    json resp = {
        {"frame", "resp"},
        {"id",    req_id},
        {"ok",    false},
        {"err",   {{"code", code}, {"msg", msg}, {"tip", tip}}},
    };
    if (hr) resp["err"]["hr"] = hr;
    if (m_pipe) m_pipe->Send(resp.dump(), generation);
}

bool Router::SendOk(std::int64_t req_id, const json& data,
                    ConnectionGeneration generation, bool final_response) {
    json resp = {
        {"frame", "resp"},
        {"id",    req_id},
        {"ok",    true},
        {"data",  data},
    };
    if (final_response) resp["terminal"] = true;
    if (!m_pipe) return false;
    const std::string payload = resp.dump();
    return final_response ? m_pipe->SendFinal(payload, generation, req_id)
                          : m_pipe->Send(payload, generation);
}

} // namespace windbgmcp::ipc
