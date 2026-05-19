// SPDX-License-Identifier: MIT
//
// Single-instance pipe server with overlapped I/O.
//
//  - One named pipe instance, one connected client at a time.
//  - Reader thread does overlapped ReadFile + WaitForMultipleObjects so it
//    can be woken to send outgoing frames or to shut down.
//  - Writer is synchronous (under a mutex). Outbound frames are short
//    enough that this is fine; events and responses both go through Send.
//  - On disconnect we close the handle and re-listen.

#pragma once

#include "ipc/frame_codec.h"

#include <Windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace windbgmcp::ipc {

// Callback the pipe server invokes when a complete payload arrives. Runs on
// the reader thread; must not block (the router handler must dispatch to a
// worker thread itself if heavy work is needed).
using OnFrame = std::function<void(std::string payload)>;

class PipeServer {
public:
    PipeServer();
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Start listening on \\.\pipe\windbgmcp. Returns false if another
    // PipeServer instance (this process or otherwise) already owns it.
    bool Start(OnFrame on_frame);

    // Stop the server and wait for the reader thread to exit. Idempotent.
    void Stop();

    bool IsRunning() const noexcept { return m_running.load(); }
    bool IsConnected() const noexcept { return m_connected.load(); }

    // Send a payload to the currently connected client. Returns false if no
    // client is connected or the write failed.
    bool Send(std::string_view payload);

private:
    void Run();
    bool TryCreatePipe();
    void HandleClient();
    void Disconnect();

    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_connected{false};
    OnFrame               m_on_frame;
    std::thread           m_thread;

    // Pipe handle: valid between successful CreateNamedPipeW and Disconnect.
    HANDLE                m_pipe   = INVALID_HANDLE_VALUE;
    HANDLE                m_evt_stop = nullptr;   // manual-reset, signals Stop()
    HANDLE                m_evt_send = nullptr;   // auto-reset, wakes reader to send

    // Outbound frames are not actually queued at this layer — Send() just
    // does a synchronous WriteFile under a mutex. The event above exists so
    // the reader can be interrupted from its WaitForMultipleObjects when it
    // needs to be torn down; we don't need to wake it for sends.
    mutable std::mutex    m_write_mu;

    Decoder               m_decoder;
};

} // namespace windbgmcp::ipc
