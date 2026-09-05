// SPDX-License-Identifier: MIT
#include "ipc/pipe_server.h"

#include "util/log.h"
#include "windbgmcp/pipe_endpoint.h"

#include <sddl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace windbgmcp::ipc {

namespace {
constexpr DWORD kReadBufferBytes = 64 * 1024;
constexpr DWORD kWriteTimeoutMs = 5'000;
constexpr DWORD kFinalCommitTimeoutMs = 4'000;
constexpr DWORD kCancelDrainMs = 250;

// Build a SECURITY_ATTRIBUTES that lets a non-elevated MCP server connect to
// a pipe owned by an elevated WinDbg.
//   D:(A;;GA;;;WD)     - DACL: Generic All to Everyone
//   S:(ML;;NW;;;LW)    - SACL: Mandatory Label Low IL, No-Write-Up
// Caller owns the returned PSECURITY_DESCRIPTOR (LocalFree).
bool BuildSecurityDescriptor(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)",
            SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    sa = {};
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = FALSE;
    return true;
}

struct PendingWrite {
    OVERLAPPED ov{};
    HANDLE event = nullptr;
    std::vector<std::uint8_t> bytes;

    ~PendingWrite() {
        if (event) ::CloseHandle(event);
    }
};

// A named-pipe peer is allowed to stop reading at any time. Keep both the
// OVERLAPPED and its backing bytes on the heap so the deadline remains real:
// if the kernel does not acknowledge CancelIoEx within the short drain grace,
// the operation storage is deliberately abandoned instead of blocking or
// returning with stack/buffer use-after-free. Closing the pipe during teardown
// still cancels that I/O; the exceptional path leaks one bounded frame only.
bool WriteAllBounded(
    HANDLE h,
    const void* data,
    DWORD bytes,
    HANDLE stop_event,
    std::chrono::steady_clock::time_point deadline) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (bytes > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::SetLastError(ERROR_TIMEOUT);
            return false;
        }
        auto pending = std::make_unique<PendingWrite>();
        pending->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!pending->event) return false;
        pending->ov.hEvent = pending->event;
        pending->bytes.assign(p, p + bytes);

        DWORD written = 0;
        BOOL ok = ::WriteFile(h, pending->bytes.data(), bytes, &written,
                              &pending->ov);
        DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
        if (!ok && error == ERROR_IO_PENDING) {
            const auto now = std::chrono::steady_clock::now();
            DWORD remaining_ms = 0;
            if (now < deadline) {
                const auto remaining = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now).count();
                remaining_ms = static_cast<DWORD>(
                    (std::min<std::int64_t>)(remaining, MAXDWORD - 1));
                if (remaining_ms == 0) remaining_ms = 1;
            }

            HANDLE waits[2] = {pending->event, stop_event};
            const DWORD wait_count = stop_event ? 2u : 1u;
            const DWORD wr = ::WaitForMultipleObjects(
                wait_count, waits, FALSE, remaining_ms);
            if (wr == WAIT_OBJECT_0) {
                ok = ::GetOverlappedResult(h, &pending->ov, &written, FALSE);
                error = ok ? ERROR_SUCCESS : ::GetLastError();
            } else {
                const DWORD terminal_error =
                    wr == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED
                                             : ERROR_TIMEOUT;
                ::CancelIoEx(h, &pending->ov);
                if (::WaitForSingleObject(pending->event, kCancelDrainMs) ==
                    WAIT_OBJECT_0) {
                    DWORD ignored = 0;
                    ::GetOverlappedResult(h, &pending->ov, &ignored, FALSE);
                } else {
                    // Preserve OVERLAPPED and buffer lifetime. See comment
                    // above; no code is scheduled to execute after return.
                    (void)pending.release();
                }
                ::SetLastError(terminal_error);
                return false;
            }
        }
        if (!ok || written == 0) {
            ::SetLastError(error);
            return false;
        }
        p     += written;
        bytes -= written;
    }
    return true;
}

} // namespace

PipeServer::PipeServer(std::string endpoint)
    : m_endpoint(std::move(endpoint)),
      m_endpoint_wide(PipeEndpointToWide(m_endpoint)) {}

PipeServer::~PipeServer() {
    Stop();
}

bool PipeServer::Start(OnFrame on_frame, std::string greeting_payload) {
    if (m_running.exchange(true)) {
        return true; // already running
    }
    m_on_frame = std::move(on_frame);
    m_greeting_payload = std::move(greeting_payload);
    m_evt_stop  = ::CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    if (!m_evt_stop) {
        m_last_error.store(::GetLastError());
        m_running.store(false);
        return false;
    }
    m_evt_final_ack = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_evt_final_ack) {
        m_last_error.store(::GetLastError());
        ::CloseHandle(m_evt_stop);
        m_evt_stop = nullptr;
        m_running.store(false);
        return false;
    }
    // Claim the first instance synchronously so !mcpext.start can report a
    // conflicting endpoint immediately instead of claiming success while a
    // background retry loop spins forever.
    if (!TryCreatePipe()) {
        ::CloseHandle(m_evt_final_ack);
        m_evt_final_ack = nullptr;
        ::CloseHandle(m_evt_stop);
        m_evt_stop = nullptr;
        m_running.store(false);
        return false;
    }
    try {
        m_thread = std::thread([this] { Run(); });
    } catch (...) {
        m_last_error.store(ERROR_NOT_ENOUGH_MEMORY);
        m_running.store(false);
        ClosePipe();
        ::CloseHandle(m_evt_final_ack);
        m_evt_final_ack = nullptr;
        ::CloseHandle(m_evt_stop);
        m_evt_stop = nullptr;
        return false;
    }
    return true;
}

void PipeServer::Stop() {
    {
        std::lock_guard<std::mutex> state_lk(m_state_mu);
        m_running.store(false);
        m_fence.Close(m_fence.Current());
        m_ready_generation.store(kNoConnectionGeneration,
                                 std::memory_order_release);
        if (m_evt_stop) ::SetEvent(m_evt_stop);
        if (m_pipe != INVALID_HANDLE_VALUE) ::CancelIoEx(m_pipe, nullptr);
    }
    if (m_thread.joinable()) m_thread.join();

    // No reader I/O remains. Wait for any writer that was cancelled above,
    // then release the endpoint exactly once.
    ClosePipe();
    if (m_evt_final_ack) {
        ::CloseHandle(m_evt_final_ack);
        m_evt_final_ack = nullptr;
    }
    if (m_evt_stop) { ::CloseHandle(m_evt_stop); m_evt_stop = nullptr; }
}

bool PipeServer::TryCreatePipe() {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    LPSECURITY_ATTRIBUTES psa = BuildSecurityDescriptor(sa, sd) ? &sa : nullptr;

    m_pipe = ::CreateNamedPipeW(
        m_endpoint_wide.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,            // single instance only
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                                            // max 1 instance
        kReadBufferBytes, kReadBufferBytes,
        0, psa);

    DWORD err = ::GetLastError();
    if (sd) ::LocalFree(sd);

    if (m_pipe == INVALID_HANDLE_VALUE) {
        m_last_error.store(err);
        WMCP_LOG(Error, std::string("CreateNamedPipeW failed for ") + m_endpoint +
                        ", gle=" + std::to_string(err));
        return false;
    }
    m_last_error.store(ERROR_SUCCESS);
    return true;
}

void PipeServer::Run() {
    while (m_running.load()) {
        // Wait for a client to connect (overlapped).
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) break;
        BOOL ok = ::ConnectNamedPipe(m_pipe, &ov);
        DWORD err = ::GetLastError();
        bool connected = false;
        if (ok || (!ok && err == ERROR_PIPE_CONNECTED)) {
            connected = true;
        } else if (!ok && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, m_evt_stop };
            DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr == WAIT_OBJECT_0) {
                DWORD got = 0;
                connected = ::GetOverlappedResult(m_pipe, &ov, &got, FALSE);
                if (!connected) err = ::GetLastError();
            } else {
                ::CancelIoEx(m_pipe, &ov);
                DWORD ignored = 0;
                ::GetOverlappedResult(m_pipe, &ov, &ignored, TRUE);
            }
        }
        ::CloseHandle(ov.hEvent);

        if (!connected || !m_running.load()) {
            if (!m_running.load()) break;

            // A client can open and close between CreateNamedPipe and
            // ConnectNamedPipe, yielding ERROR_NO_DATA. Reset this same
            // server instance and keep listening without releasing the
            // endpoint handle.
            WMCP_LOG(Warn, "ConnectNamedPipe failed, gle=" + std::to_string(err));
            ::DisconnectNamedPipe(m_pipe);
            continue;
        }

        ConnectionGeneration generation = kNoConnectionGeneration;
        {
            // Serialize a new generation with SendFinal. Once a terminal
            // response commits m_running=false, this reader can no longer
            // publish a replacement generation behind the old request.
            std::lock_guard<std::mutex> state_lk(m_state_mu);
            if (m_running.load()) generation = m_fence.Open();
        }
        if (generation == kNoConnectionGeneration) break;
        WMCP_LOG(Info, "client connected generation=" + std::to_string(generation));
        if (!m_greeting_payload.empty() &&
            !SendWithDeadline(m_greeting_payload, generation,
                              /*allow_before_greeting=*/true)) {
            WMCP_LOG(Warn,
                     "failed to send bridge identity greeting; disconnecting client");
            DisconnectClient(generation);
            continue;
        }
        m_ready_generation.store(generation, std::memory_order_release);
        HandleClient(generation);
        WMCP_LOG(Info, "client disconnected generation=" + std::to_string(generation));
        DisconnectClient(generation);
    }
    m_running.store(false);
}

void PipeServer::HandleClient(ConnectionGeneration generation) {
    std::array<std::uint8_t, kReadBufferBytes> buf{};
    m_decoder = Decoder{};

    WMCP_LOG(Info, "HandleClient: entered");
    while (m_running.load()) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return;
        DWORD got = 0;
        BOOL ok = ::ReadFile(m_pipe, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov);
        DWORD err = ::GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, m_evt_stop };
            DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr != WAIT_OBJECT_0) {
                ::CancelIoEx(m_pipe, &ov);
                DWORD ignored = 0;
                ::GetOverlappedResult(m_pipe, &ov, &ignored, TRUE);
                ::CloseHandle(ov.hEvent);
                return;
            }
            ok = ::GetOverlappedResult(m_pipe, &ov, &got, FALSE);
        }
        ::CloseHandle(ov.hEvent);

        if (!ok || got == 0) {
            WMCP_LOG(Info, std::string("HandleClient: ReadFile end ok=") +
                           std::to_string(ok) + " got=" + std::to_string(got));
            return;
        }

        WMCP_LOG(Info, std::string("HandleClient: got bytes=") + std::to_string(got));
        m_decoder.Feed({buf.data(), got});
        while (auto payload = m_decoder.TryPop()) {
            WMCP_LOG(Info, std::string("HandleClient: complete frame, dispatch to router, bytes=") +
                           std::to_string(payload->size()));
            if (!HandleControlFrame(*payload, generation) && m_on_frame) {
                m_on_frame(std::move(*payload), generation);
            }
        }
        if (m_decoder.HasError()) {
            WMCP_LOG(Error, "frame too large; disconnecting client");
            return;
        }
    }
}

void PipeServer::DisconnectClient(ConnectionGeneration generation) {
    std::unique_lock<std::mutex> state_lk(m_state_mu);
    // SendFinal reserves its generation while it performs bounded I/O. Let it
    // either commit or abort before retiring/disconnecting that generation.
    m_state_cv.wait(state_lk, [this, generation] {
        return m_finalizing_generation != generation;
    });
    m_fence.Close(generation);
    auto ready = generation;
    m_ready_generation.compare_exchange_strong(
        ready, kNoConnectionGeneration, std::memory_order_acq_rel);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        std::lock_guard<std::timed_mutex> lk(m_write_mu);
        ::DisconnectNamedPipe(m_pipe);
    }
}

bool PipeServer::HandleControlFrame(
    std::string_view payload,
    ConnectionGeneration generation) {
    std::int64_t request_id = 0;
    try {
        const auto message = nlohmann::json::parse(payload);
        if (!message.is_object() || message.value("frame", "") != "ack" ||
            !message.value("terminal", false)) {
            return false;
        }
        request_id = message.value("id", static_cast<std::int64_t>(0));
    } catch (...) {
        return false;
    }

    std::lock_guard<std::mutex> state_lk(m_state_mu);
    if (m_finalizing_generation == generation &&
        m_finalizing_request_id == request_id && m_evt_final_ack) {
        ::SetEvent(m_evt_final_ack);
    }
    // ACK frames are transport control and must never enter the dbgeng lane,
    // including late/stale acknowledgements.
    return true;
}

void PipeServer::ClosePipe() {
    std::lock_guard<std::mutex> state_lk(m_state_mu);
    m_fence.Close(m_fence.Current());
    m_ready_generation.store(kNoConnectionGeneration,
                             std::memory_order_release);
    std::lock_guard<std::timed_mutex> lk(m_write_mu);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

bool PipeServer::Send(std::string_view payload) {
    return Send(payload, m_fence.Current());
}

bool PipeServer::Send(std::string_view payload,
                      ConnectionGeneration generation) {
    return SendWithDeadline(payload, generation,
                            /*allow_before_greeting=*/false);
}

bool PipeServer::SendWithDeadline(
    std::string_view payload,
    ConnectionGeneration generation,
    bool allow_before_greeting) {
    if (!m_fence.Allows(generation)) return false;
    if (!allow_before_greeting &&
        m_ready_generation.load(std::memory_order_acquire) != generation) {
        return false;
    }
    std::vector<std::uint8_t> frame;
    if (!Encode(payload, frame)) return false;

    WMCP_LOG(Info, std::string("Send: about to write bytes=") +
                   std::to_string(frame.size()));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kWriteTimeoutMs);
    std::unique_lock<std::timed_mutex> lk(m_write_mu, std::defer_lock);
    if (!lk.try_lock_until(deadline)) {
        ::SetLastError(ERROR_TIMEOUT);
        WMCP_LOG(Warn, "Send: timed out waiting for the pipe writer");
        return false;
    }
    if (m_pipe == INVALID_HANDLE_VALUE || !m_fence.Allows(generation) ||
        (!allow_before_greeting &&
         m_ready_generation.load(std::memory_order_acquire) != generation)) {
        return false;
    }
    bool ok = WriteAllBounded(m_pipe, frame.data(),
                              static_cast<DWORD>(frame.size()),
                              m_evt_stop, deadline);
    if (!ok && m_pipe != INVALID_HANDLE_VALUE &&
        m_fence.Allows(generation)) {
        // A partial byte-stream frame poisons this generation. Cancel its
        // reader so Run retires/disconnects it before any later frame.
        ::CancelIoEx(m_pipe, nullptr);
    }
    WMCP_LOG(Info, std::string("Send: write done ok=") + std::to_string(ok));
    return ok;
}

bool PipeServer::SendFinal(std::string_view payload,
                           ConnectionGeneration generation,
                           std::int64_t request_id) {
    std::vector<std::uint8_t> frame;
    if (!Encode(payload, frame)) return false;

    {
        std::lock_guard<std::mutex> state_lk(m_state_mu);
        if (!m_running.load() || !m_fence.Allows(generation) ||
            m_finalizing_generation != kNoConnectionGeneration) {
            return false;
        }
        m_finalizing_generation = generation;
        m_finalizing_request_id = request_id;
        if (m_evt_final_ack) ::ResetEvent(m_evt_final_ack);
    }

    const auto clear_reservation = [this, generation] {
        std::lock_guard<std::mutex> state_lk(m_state_mu);
        if (m_finalizing_generation == generation) {
            m_finalizing_generation = kNoConnectionGeneration;
            m_finalizing_request_id = 0;
        }
        m_state_cv.notify_all();
    };

    WMCP_LOG(Info, std::string("SendFinal: about to write bytes=") +
                   std::to_string(frame.size()) + " generation=" +
                   std::to_string(generation));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kFinalCommitTimeoutMs);
    bool wrote = false;
    bool writable_generation = true;
    {
        std::unique_lock<std::timed_mutex> write_lk(m_write_mu,
                                                    std::defer_lock);
        if (!write_lk.try_lock_until(deadline)) {
            ::SetLastError(ERROR_TIMEOUT);
            clear_reservation();
            WMCP_LOG(Warn,
                     "SendFinal: timed out waiting for the pipe writer");
            return false;
        }
        if (m_pipe == INVALID_HANDLE_VALUE || !m_fence.Allows(generation)) {
            writable_generation = false;
        } else {
            wrote = WriteAllBounded(m_pipe, frame.data(),
                                    static_cast<DWORD>(frame.size()),
                                    m_evt_stop, deadline);
        }
    }
    if (!writable_generation) {
        clear_reservation();
        return false;
    }

    if (wrote) {
        const DWORD remaining_ms = [&] {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return DWORD{0};
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - now).count();
            return static_cast<DWORD>(
                (std::min<std::int64_t>)(remaining, MAXDWORD - 1));
        }();
        HANDLE waits[2] = {m_evt_final_ack, m_evt_stop};
        const DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, remaining_ms);
        if (wr != WAIT_OBJECT_0) {
            wrote = false;
            ::SetLastError(wr == WAIT_OBJECT_0 + 1
                               ? ERROR_OPERATION_ABORTED
                               : ERROR_TIMEOUT);
            WMCP_LOG(Warn,
                     "SendFinal: host did not acknowledge terminal response");
        }
    }

    bool committed = false;
    {
        std::lock_guard<std::mutex> state_lk(m_state_mu);
        if (wrote && m_running.load() && m_fence.Allows(generation) &&
            m_finalizing_generation == generation) {
            // Successful overlapped write completion means the full frame is
            // owned by the pipe transport. Do not call FlushFileBuffers: it
            // waits for peer consumption and has no bounded cancellation API.
            m_running.store(false);
            m_fence.Close(generation);
            m_ready_generation.store(kNoConnectionGeneration,
                                     std::memory_order_release);
            if (m_evt_stop) ::SetEvent(m_evt_stop);
            if (m_pipe != INVALID_HANDLE_VALUE) ::CancelIoEx(m_pipe, nullptr);
            committed = true;
        }
        if (m_finalizing_generation == generation) {
            m_finalizing_generation = kNoConnectionGeneration;
            m_finalizing_request_id = 0;
        }
        m_state_cv.notify_all();
    }
    if (!committed) {
        WMCP_LOG(Warn,
                 "SendFinal: terminal response did not commit; shutdown suppressed");
        return false;
    }
    WMCP_LOG(Info,
             "SendFinal: response committed; connection fenced and reader stopped");
    return true;
}

} // namespace windbgmcp::ipc
