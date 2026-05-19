// SPDX-License-Identifier: MIT
#include "transport/pipe_client.h"

#include "util/log.h"
#include "windbgmcp/protocol.h"

#include <array>
#include <chrono>

namespace wmh::transport {

using nlohmann::json;

namespace {
constexpr DWORD kReadBufBytes = 64 * 1024;
} // namespace

PipeClient::PipeClient() = default;

PipeClient::~PipeClient() { Close(); }

bool PipeClient::EnsureConnected(std::uint32_t timeout_ms) {
    // Serialise connect attempts: multiple worker threads may race here
    // when the previous connection just died and the AI has several
    // requests queued.
    std::lock_guard<std::mutex> conn_lk(m_conn_mu);

    if (m_connected.load()) return true;

    // If a previous reader exited (e.g. WinDbg was restarted and the pipe
    // server tore down), join it before reusing the std::thread slot, or
    // its destructor would call std::terminate.
    if (m_reader.joinable()) {
        m_reader.join();
    }
    if (m_pipe != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    if (m_stop_evt) {
        ::CloseHandle(m_stop_evt);
        m_stop_evt = nullptr;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        // FILE_FLAG_OVERLAPPED is mandatory: the reader thread parks in a
        // blocking ReadFile, and the writer thread issues WriteFile from a
        // different thread on the same handle. On a synchronous-mode pipe
        // handle the kernel serialises those two operations, so a reader
        // sitting in ReadFile blocks the next write indefinitely (we saw
        // this in real logs as "Request[2] about to WriteFile" with no
        // matching return). Overlapped I/O is the only way out.
        HANDLE h = ::CreateFileW(windbgmcp::kPipeName,
                                 GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            m_pipe = h;
            m_stop_evt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            // Reset the decoder so leftover bytes from a previous session
            // don't poison the new stream.
            m_decoder = Decoder{};
            m_connected.store(true);
            m_running.store(true);
            m_reader = std::thread([this] { ReaderLoop(); });
            WMCP_LOG(Info, "PipeClient: connected to ext");
            return true;
        }
        DWORD e = ::GetLastError();
        if (e == ERROR_PIPE_BUSY) {
            ::WaitNamedPipeW(windbgmcp::kPipeName, 500);
            continue;
        }
        if (e == ERROR_FILE_NOT_FOUND) {
            ::Sleep(200);
            continue;
        }
        WMCP_LOG(Warn, std::string("CreateFileW pipe failed gle=") + std::to_string(e));
        return false;
    }
    return false;
}

void PipeClient::Close() {
    bool was = m_running.exchange(false);
    if (!was) return;
    if (m_stop_evt) ::SetEvent(m_stop_evt);
    HANDLE h = m_pipe;
    m_pipe = INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        ::CancelIoEx(h, nullptr);
        ::CloseHandle(h);
    }
    if (m_reader.joinable()) m_reader.join();
    if (m_stop_evt) { ::CloseHandle(m_stop_evt); m_stop_evt = nullptr; }
    m_connected.store(false);

    FailAllPending("transport closed");
}

void PipeClient::FailAllPending(const std::string& reason) {
    std::lock_guard<std::mutex> lk(m_pending_mu);
    for (auto& kv : m_pending) {
        if (!kv.second) continue;
        try {
            kv.second->promise.set_value(
                Response{false, {},
                         Error{"disconnected", reason,
                               "WinDbg pipe gone; next call will auto-reconnect",
                               0}});
        } catch (const std::future_error&) {
            // promise already satisfied; ignore.
        }
    }
    m_pending.clear();
}

void PipeClient::ReaderLoop() {
    std::array<std::uint8_t, kReadBufBytes> buf{};
    WMCP_LOG(Trace, "ReaderLoop: entered");
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (m_running.load()) {
        OVERLAPPED ov{};
        ::ResetEvent(ev);
        ov.hEvent = ev;
        WMCP_LOG(Trace, "ReaderLoop: about to ReadFile");
        DWORD got = 0;
        BOOL  ok  = ::ReadFile(m_pipe, buf.data(),
                               static_cast<DWORD>(buf.size()),
                               &got, &ov);
        DWORD gle = ::GetLastError();
        if (!ok && gle == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ev, m_stop_evt };
            DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr != WAIT_OBJECT_0) {
                ::CancelIoEx(m_pipe, &ov);
                WMCP_LOG(Trace, "ReaderLoop: stop event signalled");
                break;
            }
            ok  = ::GetOverlappedResult(m_pipe, &ov, &got, FALSE);
            gle = ::GetLastError();
        }
        WMCP_LOG(Trace, std::string("ReaderLoop: ReadFile/GOR ok=") + std::to_string(ok) +
                        " got=" + std::to_string(got) +
                        " gle=" + std::to_string(gle));
        if (!ok || got == 0) {
            if (gle != 0 && gle != ERROR_OPERATION_ABORTED) {
                WMCP_LOG(Warn, std::string("ReadFile pipe failed gle=") + std::to_string(gle));
            }
            break;
        }
        m_decoder.Feed({buf.data(), got});
        while (auto payload = m_decoder.TryPop()) {
            WMCP_LOG(Trace, std::string("ReaderLoop: dispatching payload bytes=") +
                            std::to_string(payload->size()));
            Dispatch(*payload);
        }
        if (m_decoder.HasError()) {
            WMCP_LOG(Error, "decoder: frame too large; closing");
            break;
        }
    }
    if (ev) ::CloseHandle(ev);
    WMCP_LOG(Trace, "ReaderLoop: exiting");
    m_connected.store(false);

    // Fail any in-flight requests immediately so callers don't hang for
    // their per-request timeout. The next Request will trigger
    // EnsureConnected -> reconnect.
    FailAllPending("pipe disconnected (WinDbg/ext gone)");
}

void PipeClient::Dispatch(const std::string& payload) {
    json msg;
    try {
        msg = json::parse(payload);
    } catch (...) {
        WMCP_LOG(Warn, "invalid JSON from ext");
        return;
    }
    const std::string frame = msg.value("frame", "");
    if (frame == "resp") {
        const auto id = msg.value("id", static_cast<std::int64_t>(0));
        std::unique_ptr<Pending> slot;
        {
            std::lock_guard<std::mutex> lk(m_pending_mu);
            auto it = m_pending.find(id);
            if (it != m_pending.end()) {
                slot = std::move(it->second);
                m_pending.erase(it);
            }
        }
        if (!slot) return;
        if (msg.value("ok", false)) {
            Response r;
            r.ok = true;
            r.data = msg.value("data", json::object());
            if (!slot->chunks.empty() && !slot->sink) {
                r.data["_chunks"] = slot->chunks;
            }
            slot->promise.set_value(std::move(r));
        } else {
            auto e = msg.value("err", json::object());
            Response r;
            r.ok = false;
            r.err.code = e.value("code", "engine_error");
            r.err.msg  = e.value("msg",  "no message");
            r.err.tip  = e.value("tip",  "");
            r.err.hr   = e.value("hr",   static_cast<long>(0));
            slot->promise.set_value(std::move(r));
        }
        return;
    }
    if (frame == "chunk") {
        const auto id = msg.value("id", static_cast<std::int64_t>(0));
        const std::string chunk = msg.value("chunk", "");
        const bool eof = msg.value("eof", false);
        std::lock_guard<std::mutex> lk(m_pending_mu);
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        if (it->second->sink) {
            it->second->sink(chunk, eof);
        } else {
            it->second->chunks.push_back(chunk);
        }
        return;
    }
    if (frame == "event") {
        std::vector<EventHandler> handlers;
        {
            std::lock_guard<std::mutex> lk(m_subs_mu);
            handlers.reserve(m_subs.size());
            for (auto& kv : m_subs) handlers.push_back(kv.second);
        }
        for (auto& h : handlers) h(msg);
        return;
    }
}

std::uint64_t PipeClient::Subscribe(EventHandler handler) {
    auto id = m_next_sub.fetch_add(1);
    std::lock_guard<std::mutex> lk(m_subs_mu);
    m_subs[id] = std::move(handler);
    return id;
}

void PipeClient::Unsubscribe(std::uint64_t token) {
    std::lock_guard<std::mutex> lk(m_subs_mu);
    m_subs.erase(token);
}

Response PipeClient::Request(const std::string& op,
                             const json& args,
                             std::uint32_t timeout_ms,
                             ChunkSink chunk_sink) {
    if (!EnsureConnected()) {
        return Response{false, {}, Error{"disconnected",
            "could not open \\\\.\\pipe\\windbgmcp",
            "in WinDbg run: .load windbgmcpExt; !mcpext.start", 0}};
    }
    const auto id = m_next_id.fetch_add(1);
    auto slot = std::make_unique<Pending>();
    auto fut = slot->promise.get_future();
    slot->sink = std::move(chunk_sink);
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending[id] = std::move(slot);
    }

    json frame = {
        {"frame", "req"},
        {"id",    id},
        {"op",    op},
        {"args",  args},
    };
    std::string body = frame.dump();
    std::vector<std::uint8_t> wire;
    if (!Encode(body, wire)) {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending.erase(id);
        return Response{false, {}, Error{"protocol_error", "frame too large", "", 0}};
    }
    WMCP_LOG(Trace, "Request[" + std::to_string(id) + "] op=" + op +
                    " bytes=" + std::to_string(wire.size()) + " about to WriteFile");
    {
        std::lock_guard<std::mutex> lk(m_write_mu);
        if (m_pipe == INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> p(m_pending_mu);
            m_pending.erase(id);
            return Response{false, {}, Error{"disconnected", "pipe closed", "", 0}};
        }
        // Overlapped write: submit + wait for completion. This is required
        // because the pipe handle is OVERLAPPED-mode (so a concurrent
        // ReadFile on the reader thread doesn't block us).
        OVERLAPPED ov{};
        HANDLE wev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ov.hEvent = wev;
        DWORD written = 0;
        BOOL  wf_ok   = ::WriteFile(m_pipe, wire.data(),
                                    static_cast<DWORD>(wire.size()),
                                    &written, &ov);
        DWORD wf_err  = ::GetLastError();
        if (!wf_ok && wf_err == ERROR_IO_PENDING) {
            wf_ok  = ::GetOverlappedResult(m_pipe, &ov, &written, TRUE);
            wf_err = ::GetLastError();
        }
        if (wev) ::CloseHandle(wev);
        WMCP_LOG(Trace, "Request[" + std::to_string(id) + "] WriteFile returned ok=" +
                        std::to_string(wf_ok) + " written=" + std::to_string(written) +
                        " gle=" + std::to_string(wf_err));
        if (!wf_ok || written != wire.size()) {
            std::lock_guard<std::mutex> p(m_pending_mu);
            m_pending.erase(id);
            return Response{false, {}, Error{"disconnected", "WriteFile failed", "", 0}};
        }
    }
    WMCP_LOG(Trace, "Request[" + std::to_string(id) + "] waiting for future, timeout_ms=" +
                    std::to_string(timeout_ms));
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) ==
        std::future_status::timeout) {
        WMCP_LOG(Warn, "Request[" + std::to_string(id) + "] TIMED OUT waiting for resp");
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending.erase(id);
        return Response{false, {}, Error{"timeout",
            "no response from ext within " + std::to_string(timeout_ms) + "ms",
            "raise timeout or check if WinDbg is responsive", 0}};
    }
    WMCP_LOG(Trace, "Request[" + std::to_string(id) + "] future ready, returning");
    return fut.get();
}

} // namespace wmh::transport
