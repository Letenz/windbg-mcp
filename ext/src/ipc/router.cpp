// SPDX-License-Identifier: MIT
#include "ipc/router.h"

#include "ipc/pipe_server.h"
#include "util/log.h"
#include "windbgmcp/protocol.h"

namespace windbgmcp::ipc {

using nlohmann::json;

Router::Router() = default;

Router::~Router() {
    Stop();
}

void Router::Register(const std::string& op, Handler h) {
    m_handlers[op] = std::move(h);
}

void Router::Start(std::size_t worker_count) {
    if (m_running.exchange(true)) return;
    m_workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        m_workers.emplace_back([this] { WorkerLoop(); });
    }
}

void Router::Stop() {
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    // Some unload paths (notably WinDbg's `.reboot` synchronously calling
    // DebugExtensionUninitialize from inside our handler's Execute) reach
    // Stop() while the current thread *is* one of m_workers. Calling
    // join() on the current std::thread throws "resource deadlock would
    // occur". Detach in that case; the worker loop will see m_running=false
    // immediately after the in-flight handler returns and exit on its own.
    const auto self_id = std::this_thread::get_id();
    for (auto& t : m_workers) {
        if (!t.joinable()) continue;
        if (t.get_id() == self_id) {
            WMCP_LOG(Warn, "Router::Stop called from within a worker; detaching self");
            t.detach();
        } else {
            t.join();
        }
    }
    m_workers.clear();
    std::queue<std::string> empty;
    std::lock_guard<std::mutex> lk(m_mu);
    std::swap(m_queue, empty);
}

void Router::OnPayload(std::string payload) {
    WMCP_LOG(Info, std::string("Router::OnPayload: enqueue bytes=") +
                   std::to_string(payload.size()));
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_queue.push(std::move(payload));
    }
    m_cv.notify_one();
}

void Router::WorkerLoop() {
    while (m_running.load()) {
        std::string payload;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] { return !m_queue.empty() || !m_running.load(); });
            if (!m_running.load() && m_queue.empty()) return;
            payload = std::move(m_queue.front());
            m_queue.pop();
        }
        WMCP_LOG(Info, std::string("WorkerLoop: dispatching, bytes=") +
                       std::to_string(payload.size()));
        Dispatch(std::move(payload));
        WMCP_LOG(Info, "WorkerLoop: dispatch returned");
    }
}

void Router::Dispatch(std::string payload) {
    json msg;
    std::int64_t req_id = 0;
    try {
        msg = json::parse(payload);
    } catch (const std::exception& e) {
        WMCP_LOG(Warn, std::string("invalid JSON frame: ") + e.what());
        // No id available — send a synthetic error with id 0.
        SendError(0, err::kProtocolError, "invalid JSON in frame", "fix client");
        return;
    }

    if (!msg.is_object() || msg.value("frame", "") != "req") {
        SendError(0, err::kProtocolError, "expected frame=req", "");
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
                  "see docs/protocol.md");
        return;
    }

    WMCP_LOG(Info, std::string("Dispatch: calling handler op=") + op +
                   " id=" + std::to_string(req_id));
    try {
        json data = it->second(req_id, args, *m_pipe);
        WMCP_LOG(Info, std::string("Dispatch: handler returned op=") + op +
                       " id=" + std::to_string(req_id));
        SendOk(req_id, data);
    } catch (const HandlerError& e) {
        WMCP_LOG(Warn, std::string("Dispatch: handler error op=") + op +
                       " code=" + e.code());
        SendError(req_id, e.code(), e.msg(), e.tip(), e.hr());
    } catch (const std::exception& e) {
        WMCP_LOG(Error, std::string("Dispatch: handler threw op=") + op +
                        " what=" + e.what());
        SendError(req_id, err::kEngineError,
                  std::string("handler threw: ") + e.what(),
                  "check ext logs");
    }
}

void Router::SendError(std::int64_t req_id, const std::string& code,
                       const std::string& msg, const std::string& tip,
                       long hr) {
    json resp = {
        {"frame", "resp"},
        {"id",    req_id},
        {"ok",    false},
        {"err",   {{"code", code}, {"msg", msg}, {"tip", tip}}},
    };
    if (hr) resp["err"]["hr"] = hr;
    if (m_pipe) m_pipe->Send(resp.dump());
}

void Router::SendOk(std::int64_t req_id, const json& data) {
    json resp = {
        {"frame", "resp"},
        {"id",    req_id},
        {"ok",    true},
        {"data",  data},
    };
    if (m_pipe) m_pipe->Send(resp.dump());
}

} // namespace windbgmcp::ipc
