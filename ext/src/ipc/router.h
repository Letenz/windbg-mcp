// SPDX-License-Identifier: MIT
//
// Request router. Owns a thread pool of worker threads. Frames arrive from
// the pipe layer; if the payload is a `req`, the router enqueues it for a
// worker that calls the registered handler and writes a `resp`. If the
// payload is anything else (no expected client-to-server frames in v1) it
// emits an `invalid_arg` error.
//
// Handlers are registered by op name. A handler returns a JSON value to
// place in `resp.data` on success, or throws windbgmcp::HandlerError to
// produce a structured error response.

#pragma once

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
                                             PipeServer& pipe)>;

class Router {
public:
    Router();
    ~Router();

    void Register(const std::string& op, Handler h);

    // Wire up the pipe so the router can send responses. Must be called
    // before Start().
    void SetPipe(PipeServer* pipe) { m_pipe = pipe; }

    void Start(std::size_t worker_count = 4);
    void Stop();

    // PipeServer's OnFrame callback: hand it raw payloads.
    void OnPayload(std::string payload);

private:
    void WorkerLoop();
    void Dispatch(std::string payload);

    void SendError(std::int64_t req_id, const std::string& code,
                   const std::string& msg, const std::string& tip = "",
                   long hr = 0);
    void SendOk(std::int64_t req_id, const nlohmann::json& data);

    PipeServer*                                 m_pipe = nullptr;
    std::unordered_map<std::string, Handler>    m_handlers;
    std::vector<std::thread>                    m_workers;
    std::queue<std::string>                     m_queue;
    std::mutex                                  m_mu;
    std::condition_variable                     m_cv;
    std::atomic<bool>                           m_running{false};
};

} // namespace windbgmcp::ipc
