// SPDX-License-Identifier: MIT
//
// Request router. Frames arrive from the pipe layer; if the payload is a
// `req`, the router enqueues it for the single dbgeng request lane, which
// calls the registered handler and writes a `resp`. If the
// payload is anything else (no expected client-to-server frames in v1) it
// emits an `invalid_arg` error.
//
// Handlers are registered by op name. A handler returns a JSON value to
// place in `resp.data` on success, or throws windbgmcp::HandlerError to
// produce a structured error response.

#pragma once

#include "ipc/connection_fence.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace windbgmcp::ipc {

class PipeServer; // forward

// Throw from a handler to produce a structured error response.
class HandlerError : public std::runtime_error {
public:
    HandlerError(std::string code, std::string msg, std::string tip = "",
                 long hr = 0)
        : std::runtime_error(msg),
          m_code(std::move(code)),
          m_msg(std::move(msg)),
          m_tip(std::move(tip)),
          m_hr(hr) {}

    const std::string& code() const noexcept { return m_code; }
    const std::string& msg()  const noexcept { return m_msg;  }
    const std::string& tip()  const noexcept { return m_tip;  }
    long               hr()   const noexcept { return m_hr;   }

private:
    std::string m_code;
    std::string m_msg;
    std::string m_tip;
    long        m_hr;
};

// Handler signature. Receives the parsed `args` object (always an object,
// possibly empty). Returns the value to place in `resp.data`.
//
// `req_id` is provided so streaming handlers (run_cmd with output_file) can
// emit `chunk` frames before returning.
using Handler = std::function<nlohmann::json(std::int64_t req_id,
                                             const nlohmann::json& args,
                                             PipeServer& pipe,
                                             ConnectionGeneration generation)>;

// Invoked only after a generation-fenced final response has been written and
// ACKed by the host, and the PipeServer has stopped accepting connections.
// The callback runs on the Router worker and therefore may only *schedule*
// teardown; it must never join or destroy the Router synchronously.
using FinalResponseAction = std::function<void(ConnectionGeneration generation)>;

class Router {
public:
    Router();
    ~Router();

    void Register(const std::string& op, Handler h);

    // Mark an operation as terminal for the current bridge instance. Its
    // success response is sent through PipeServer::SendFinal(), then action
    // is invoked. Error responses never stop the bridge.
    void RegisterFinalResponseAction(const std::string& op,
                                     FinalResponseAction action);

    // Wire up the pipe so the router can send responses. Must be called
    // before Start().
    void SetPipe(PipeServer* pipe) { m_pipe = pipe; }

    // worker_count is retained for source compatibility but is clamped to
    // one: IDebugClient objects are thread-affine and all engine operations
    // must stay on the same stable worker.
    void Start(std::size_t worker_count = 1);

    // Open the request gate only after the owner has atomically published all
    // session resources and committed lifecycle=Running. Payloads received
    // before this call are rejected on the worker without invoking handlers.
    void CommitStart() noexcept { m_accepting.store(true, std::memory_order_release); }

    // Request stop and join workers. Returns false when called from the
    // worker itself; in that case the owner must retain this Router and call
    // Stop() later from another thread before destroying it.
    bool Stop();
    bool IsWorkerThread() const noexcept;

    // PipeServer's OnFrame callback: hand it raw payloads.
    void OnPayload(std::string payload, ConnectionGeneration generation);

private:
    struct QueuedPayload {
        std::string          payload;
        ConnectionGeneration generation = kNoConnectionGeneration;
        bool                 accepted_after_commit = false;
    };

    void WorkerLoop();
    void Dispatch(std::string payload, ConnectionGeneration generation);
    void RejectBeforeCommit(std::string payload,
                            ConnectionGeneration generation);
    void RejectQueueFull(std::string payload,
                         ConnectionGeneration generation);

    void SendError(std::int64_t req_id, const std::string& code,
                   const std::string& msg, const std::string& tip = "",
                   long hr = 0,
                   ConnectionGeneration generation = kNoConnectionGeneration);
    bool SendOk(std::int64_t req_id, const nlohmann::json& data,
                ConnectionGeneration generation, bool final_response = false);

    PipeServer*                                 m_pipe = nullptr;
    std::unordered_map<std::string, Handler>    m_handlers;
    std::unordered_map<std::string, FinalResponseAction> m_final_actions;
    std::vector<std::thread>                    m_workers;
    std::queue<QueuedPayload>                   m_queue;
    std::mutex                                  m_mu;
    std::condition_variable                     m_cv;
    std::atomic<bool>                           m_running{false};
    std::atomic<bool>                           m_accepting{false};
    static constexpr std::size_t                kQueueCapacity = 256;
};

} // namespace windbgmcp::ipc
