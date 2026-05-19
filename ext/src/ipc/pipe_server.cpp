// SPDX-License-Identifier: MIT
#include "ipc/pipe_server.h"

#include "util/log.h"
#include "windbgmcp/protocol.h"

#include <sddl.h>
#include <array>
#include <vector>

namespace windbgmcp::ipc {

namespace {
constexpr DWORD kReadBufferBytes = 64 * 1024;

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

bool WriteAll(HANDLE h, const void* data, DWORD bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (bytes > 0) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD written = 0;
        BOOL ok = ::WriteFile(h, p, bytes, &written, &ov);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            ok = ::GetOverlappedResult(h, &ov, &written, TRUE);
        }
        ::CloseHandle(ov.hEvent);
        if (!ok || written == 0) return false;
        p     += written;
        bytes -= written;
    }
    return true;
}

} // namespace

PipeServer::PipeServer() = default;

PipeServer::~PipeServer() {
    Stop();
}

bool PipeServer::Start(OnFrame on_frame) {
    if (m_running.exchange(true)) {
        return true; // already running
    }
    m_on_frame  = std::move(on_frame);
    m_evt_stop  = ::CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    m_evt_send  = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_evt_stop || !m_evt_send) {
        m_running.store(false);
        return false;
    }
    m_thread = std::thread([this] { Run(); });
    return true;
}

void PipeServer::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_evt_stop) ::SetEvent(m_evt_stop);
    Disconnect();
    if (m_thread.joinable()) m_thread.join();
    if (m_evt_stop) { ::CloseHandle(m_evt_stop); m_evt_stop = nullptr; }
    if (m_evt_send) { ::CloseHandle(m_evt_send); m_evt_send = nullptr; }
}

bool PipeServer::TryCreatePipe() {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    LPSECURITY_ATTRIBUTES psa = BuildSecurityDescriptor(sa, sd) ? &sa : nullptr;

    m_pipe = ::CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,            // single instance only
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                                            // max 1 instance
        kReadBufferBytes, kReadBufferBytes,
        0, psa);

    DWORD err = ::GetLastError();
    if (sd) ::LocalFree(sd);

    if (m_pipe == INVALID_HANDLE_VALUE) {
        WMCP_LOG(Error, std::string("CreateNamedPipeW failed, gle=") + std::to_string(err));
        return false;
    }
    return true;
}

void PipeServer::Run() {
    while (m_running.load()) {
        if (!TryCreatePipe()) {
            ::Sleep(500);
            continue;
        }

        // Wait for a client to connect (overlapped).
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = ::ConnectNamedPipe(m_pipe, &ov);
        DWORD err = ::GetLastError();
        bool connected = false;
        if (!ok && err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (!ok && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, m_evt_stop };
            DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr == WAIT_OBJECT_0) {
                DWORD got = 0;
                connected = ::GetOverlappedResult(m_pipe, &ov, &got, FALSE);
            }
        }
        ::CloseHandle(ov.hEvent);

        if (!connected || !m_running.load()) {
            Disconnect();
            continue;
        }

        m_connected.store(true);
        WMCP_LOG(Info, "client connected");
        HandleClient();
        WMCP_LOG(Info, "client disconnected");
        Disconnect();
    }
}

void PipeServer::HandleClient() {
    std::array<std::uint8_t, kReadBufferBytes> buf{};
    m_decoder = Decoder{};

    WMCP_LOG(Info, "HandleClient: entered");
    while (m_running.load()) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0;
        BOOL ok = ::ReadFile(m_pipe, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov);
        DWORD err = ::GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, m_evt_stop };
            DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr != WAIT_OBJECT_0) {
                ::CancelIoEx(m_pipe, &ov);
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
            if (m_on_frame) m_on_frame(std::move(*payload));
        }
        if (m_decoder.HasError()) {
            WMCP_LOG(Error, "frame too large; disconnecting client");
            return;
        }
    }
}

void PipeServer::Disconnect() {
    m_connected.store(false);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        ::DisconnectNamedPipe(m_pipe);
        ::CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

bool PipeServer::Send(std::string_view payload) {
    if (!m_connected.load()) return false;
    std::vector<std::uint8_t> frame;
    if (!Encode(payload, frame)) return false;

    WMCP_LOG(Info, std::string("Send: about to write bytes=") +
                   std::to_string(frame.size()));
    std::lock_guard<std::mutex> lk(m_write_mu);
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    bool ok = WriteAll(m_pipe, frame.data(), static_cast<DWORD>(frame.size()));
    WMCP_LOG(Info, std::string("Send: write done ok=") + std::to_string(ok));
    return ok;
}

} // namespace windbgmcp::ipc
