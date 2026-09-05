// SPDX-License-Identifier: MIT
//
// Per-endpoint pipe server with overlapped I/O.
//
//  - One named pipe instance, one connected client at a time.
//  - Reader thread does overlapped ReadFile + WaitForMultipleObjects so it
//    can be woken to send outgoing frames or to shut down.
//  - Writers use overlapped I/O with a hard deadline and the server stop
//    event. A peer that stops reading cannot pin a Router/publisher forever.
//  - On disconnect we close the handle and re-listen.

#pragma once

#include "ipc/connection_fence.h"
#include "ipc/frame_codec.h"

#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace windbgmcp::ipc {

// Callback the pipe server invokes when a complete payload arrives. Runs on
// the reader thread; must not block (the router handler must dispatch to a
// worker thread itself if heavy work is needed).
using OnFrame = std::function<void(std::string payload,
                                   ConnectionGeneration generation)>;

class PipeServer {
public:
    explicit PipeServer(std::string endpoint);
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Start listening on the configured endpoint. Returns false if another
    // PipeServer instance (this process or otherwise) already owns it.
    // greeting_payload, when non-empty, is the first frame emitted for every
    // connection generation. Production uses it to pin the host to this
    // exact bridge instance before any request is accepted.
    bool Start(OnFrame on_frame, std::string greeting_payload = {});

    // Stop the server and wait for the reader thread to exit. Idempotent.
    void Stop();

    bool IsRunning() const noexcept { return m_running.load(); }
    bool IsConnected() const noexcept {
        return m_fence.Current() != kNoConnectionGeneration;
    }
    const std::string& Endpoint() const noexcept { return m_endpoint; }
    DWORD LastError() const noexcept { return m_last_error.load(); }
    ConnectionGeneration CurrentGeneration() const noexcept {
        return m_fence.Current();
    }
    bool IsCurrentGeneration(ConnectionGeneration generation) const noexcept {
        return m_fence.Allows(generation);
    }

    // Send a payload to the currently connected client. Returns false if no
    // client is connected or the write failed.
    bool Send(std::string_view payload);

    // Generation-fenced send for request responses/chunks. Data from an old
    // client can never land on a replacement connection.
    bool Send(std::string_view payload, ConnectionGeneration generation);

    // Write the terminal response for generation in full within a bounded
    // deadline, then atomically fence that generation and stop
    // accepting/reading connections. This is the only transport path used by
    // wm_shutdown: teardown is scheduled only after this method succeeds.
    // Named-pipe write completion is the commit point; this method never uses
    // FlushFileBuffers because a non-reading peer can make that call unbounded.
    bool SendFinal(std::string_view payload, ConnectionGeneration generation,
                   std::int64_t request_id = 0);

private:
    void Run();
    bool TryCreatePipe();
    bool SendWithDeadline(std::string_view payload,
                          ConnectionGeneration generation,
                          bool allow_before_greeting);
    void HandleClient(ConnectionGeneration generation);
    bool HandleControlFrame(std::string_view payload,
                            ConnectionGeneration generation);
    void DisconnectClient(ConnectionGeneration generation);
    void ClosePipe();

    std::atomic<bool>     m_running{false};
    std::atomic<DWORD>    m_last_error{ERROR_SUCCESS};
    ConnectionFence       m_fence;
    std::atomic<ConnectionGeneration> m_ready_generation{
        kNoConnectionGeneration};
    std::string           m_endpoint;
    std::wstring          m_endpoint_wide;
    std::string           m_greeting_payload;
    OnFrame               m_on_frame;
    std::thread           m_thread;

    // The server handle remains open across client disconnects, retaining
    // ownership of the endpoint until Stop() completes.
    HANDLE                m_pipe   = INVALID_HANDLE_VALUE;
    HANDLE                m_evt_stop = nullptr;   // manual-reset, signals Stop()
    HANDLE                m_evt_final_ack = nullptr; // host consumed terminal resp

    // Outbound frames are not queued at this layer. Send() performs an
    // overlapped WriteFile with a deadline under this mutex. It is timed so a
    // terminal response is not stuck behind a backlog of ordinary writers.
    // m_state_mu serializes connection publication/retirement and the short
    // terminal-write reservation. It is never held while waiting on I/O.
    mutable std::mutex    m_state_mu;
    std::condition_variable m_state_cv;
    ConnectionGeneration m_finalizing_generation = kNoConnectionGeneration;
    std::int64_t          m_finalizing_request_id = 0;
    mutable std::timed_mutex m_write_mu;

    Decoder               m_decoder;
};

} // namespace windbgmcp::ipc
