// SPDX-License-Identifier: MIT
//
// Multiplexed client to \\.\pipe\windbgmcp.
//
// Design:
//
//   - One pipe handle, opened lazily on first request.
//   - A dedicated reader thread loops on ReadFile, feeds the decoder, and
//     dispatches frames:
//       * `resp` / `chunk`  -> fulfil the pending Request slot keyed by id
//       * `event`           -> push onto the event subscriber list
//   - Synchronous Request() with a millisecond timeout. The handler blocks
//     on a std::future fed by the reader thread.
//   - Optional chunk_sink<chunk_text, eof>: bypasses the per-id buffer and
//     streams chunks to the caller (used by run_cmd with output_file).
//
// The pipe is read in BYTE mode (matches the ext side). Length-prefixing is
// recovered from the bytestream by the Decoder.

#pragma once

#include "transport/frame_codec.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <Windows.h>

namespace wmh::transport {

struct Error {
    std::string code;
    std::string msg;
    std::string tip;
    long        hr = 0;
};

struct Response {
    bool                ok = false;
    nlohmann::json      data;          // valid when ok
    Error               err;           // valid when !ok
};

using ChunkSink = std::function<void(std::string_view chunk, bool eof)>;
using EventHandler = std::function<void(const nlohmann::json& event_frame)>;

class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Connects to the pipe if not already connected. Returns false and
    // leaves the connection state untouched on failure. Safe to call from
    // any thread.
    bool EnsureConnected(std::uint32_t timeout_ms = 5'000);

    bool IsConnected() const noexcept { return m_connected.load(); }

    void Close();

    // Subscribe to all `event` frames. Returns a token usable with
    // Unsubscribe. The handler is invoked on the reader thread; do not
    // block.
    std::uint64_t Subscribe(EventHandler handler);
    void Unsubscribe(std::uint64_t token);

    // Synchronously send a request and wait for the response. If
    // `chunk_sink` is provided, intermediate `chunk` frames are streamed
    // through it; otherwise they are buffered into Response.data._chunks.
    Response Request(const std::string& op,
                     const nlohmann::json& args = nlohmann::json::object(),
                     std::uint32_t timeout_ms = 30'000,
                     ChunkSink chunk_sink = {});

private:
    struct Pending {
        std::promise<Response>      promise;
        std::vector<std::string>    chunks;
        ChunkSink                   sink;
    };

    void ReaderLoop();
    void Dispatch(const std::string& payload);
    void Fail(Pending* slot, const Error& err);
    void FailAllPending(const std::string& reason);

    HANDLE                              m_pipe{INVALID_HANDLE_VALUE};
    HANDLE                              m_stop_evt{nullptr};   // signal Close()
    std::atomic<bool>                   m_connected{false};
    std::atomic<bool>                   m_running{false};
    std::thread                         m_reader;
    std::mutex                          m_conn_mu;    // serialises connect/disconnect
    std::mutex                          m_write_mu;
    Decoder                             m_decoder;

    std::mutex                          m_pending_mu;
    std::map<std::int64_t, std::unique_ptr<Pending>> m_pending;
    std::atomic<std::int64_t>           m_next_id{1};

    std::mutex                          m_subs_mu;
    std::map<std::uint64_t, EventHandler> m_subs;
    std::atomic<std::uint64_t>          m_next_sub{1};
};

} // namespace wmh::transport
