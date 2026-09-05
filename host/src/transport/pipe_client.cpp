// SPDX-License-Identifier: MIT
#include "transport/pipe_client.h"

#include "util/log.h"
#include "windbgmcp/pipe_endpoint.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>

namespace wmh::transport {

using nlohmann::json;

namespace {
constexpr DWORD kReadBufBytes = 64 * 1024;
constexpr DWORD kCancelDrainMs = 250;

struct PendingWrite {
    OVERLAPPED ov{};
    HANDLE event = nullptr;
    std::vector<std::uint8_t> bytes;

    ~PendingWrite() {
        if (event) ::CloseHandle(event);
    }
};

std::uint32_t RemainingMs(
    std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return static_cast<std::uint32_t>(
        (std::min<std::int64_t>)(remaining > 0 ? remaining : 1, MAXDWORD - 1));
}

// Keep the OVERLAPPED and its bytes alive independently of Request's stack.
// If CancelIoEx is not acknowledged during the bounded drain grace, abandon
// this single allocation instead of blocking past the request deadline or
// returning with in-flight I/O pointing at freed memory.
bool WriteAllBounded(
    HANDLE pipe,
    const void* data,
    DWORD bytes,
    HANDLE stop_event,
    std::chrono::steady_clock::time_point deadline) {
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    while (bytes > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::SetLastError(ERROR_TIMEOUT);
            return false;
        }
        auto pending = std::make_unique<PendingWrite>();
        pending->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!pending->event) return false;
        pending->ov.hEvent = pending->event;
        pending->bytes.assign(cursor, cursor + bytes);

        DWORD written = 0;
        BOOL ok = ::WriteFile(pipe, pending->bytes.data(), bytes, &written,
                              &pending->ov);
        DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
        if (!ok && error == ERROR_IO_PENDING) {
            const DWORD remaining_ms = RemainingMs(deadline);
            HANDLE waits[2] = {pending->event, stop_event};
            const DWORD wait_count = stop_event ? 2u : 1u;
            const DWORD wr = ::WaitForMultipleObjects(
                wait_count, waits, FALSE, remaining_ms);
            if (wr == WAIT_OBJECT_0) {
                ok = ::GetOverlappedResult(
                    pipe, &pending->ov, &written, FALSE);
                error = ok ? ERROR_SUCCESS : ::GetLastError();
            } else {
                const DWORD terminal_error =
                    wr == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED
                                             : ERROR_TIMEOUT;
                ::CancelIoEx(pipe, &pending->ov);
                if (::WaitForSingleObject(pending->event, kCancelDrainMs) ==
                    WAIT_OBJECT_0) {
                    DWORD ignored = 0;
                    ::GetOverlappedResult(
                        pipe, &pending->ov, &ignored, FALSE);
                } else {
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
        cursor += written;
        bytes -= written;
    }
    return true;
}
} // namespace

PipeClient::PipeClient()
    : PipeClient(windbgmcp::kDefaultPipeEndpoint) {}

PipeClient::PipeClient(std::string pipe_endpoint)
    : m_endpoint(std::move(pipe_endpoint)),
      m_endpoint_wide(windbgmcp::PipeEndpointToWide(m_endpoint)) {}

PipeClient::~PipeClient() { Close(); }

bool PipeClient::EnsureConnected(std::uint32_t timeout_ms) {
    // Serialise connect attempts: multiple worker threads may race here
    // when the previous connection just died and the AI has several
    // requests queued.
    std::lock_guard<std::mutex> conn_lk(m_conn_mu);

    if (m_closed.load()) return false;
    if (m_connected.load()) return true;

    // Join and close the previous generation before publishing another.
    // Holding m_conn_mu serializes EnsureConnected against Close.
    if (!ResetConnectionLocked()) return false;

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
        HANDLE h = ::CreateFileW(m_endpoint_wide.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            if (!::SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
                ::CloseHandle(h);
                return false;
            }
            HANDLE stop_evt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stop_evt) {
                ::CloseHandle(h);
                return false;
            }

            const auto generation = m_generation_counter.fetch_add(1) + 1;
            {
                // Old Request calls may still be waiting to acquire this
                // mutex. Publish handle + generation atomically with respect
                // to writes so they cannot write an old frame to this pipe.
                std::lock_guard<std::timed_mutex> write_lk(m_write_mu);
                m_pipe = h;
                m_stop_evt = stop_evt;
                m_active_generation.store(generation);
            }
            {
                std::lock_guard<std::mutex> identity_lk(m_identity_mu);
                m_observed_instance_id.clear();
            }
            // Reset the decoder so leftover bytes from a previous session
            // don't poison the new stream.
            m_decoder = Decoder{};
            m_connected.store(true);
            m_running.store(true);
            try {
                m_reader = std::thread([this, generation] { ReaderLoop(generation); });
            } catch (...) {
                ResetConnectionLocked();
                return false;
            }
            WMCP_LOG(Info, std::string("PipeClient: connected to ext at ") + m_endpoint);
            return true;
        }
        DWORD e = ::GetLastError();
        if (e == ERROR_PIPE_BUSY) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - now).count();
            ::WaitNamedPipeW(m_endpoint_wide.c_str(), static_cast<DWORD>(
                (std::min<std::int64_t>)(remaining, 500)));
            continue;
        }
        if (e == ERROR_FILE_NOT_FOUND) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - now).count();
            ::Sleep(static_cast<DWORD>(
                (std::min<std::int64_t>)(remaining, 200)));
            continue;
        }
        WMCP_LOG(Warn, std::string("CreateFileW pipe failed gle=") + std::to_string(e));
        return false;
    }
    return false;
}

void PipeClient::Close() {
    {
        std::lock_guard<std::mutex> conn_lk(m_conn_mu);
        m_closed.store(true);
        ResetConnectionLocked();
    }
    FailAllPending("transport closed");
}

bool PipeClient::ResetConnectionLocked() {
    m_running.store(false);
    m_connected.store(false);
    m_active_generation.store(0);

    if (m_stop_evt) ::SetEvent(m_stop_evt);
    if (m_pipe != INVALID_HANDLE_VALUE) ::CancelIoEx(m_pipe, nullptr);

    if (m_reader.joinable()) {
        if (m_reader.get_id() == std::this_thread::get_id()) {
            // Never detach: the owner must call Close from a non-reader
            // thread before destruction.
            return false;
        }
        m_reader.join();
    }

    {
        std::lock_guard<std::timed_mutex> write_lk(m_write_mu);
        if (m_pipe != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
    }
    if (m_stop_evt) {
        ::CloseHandle(m_stop_evt);
        m_stop_evt = nullptr;
    }
    {
        std::lock_guard<std::mutex> identity_lk(m_identity_mu);
        m_observed_instance_id.clear();
        m_identity_cv.notify_all();
    }
    return true;
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

void PipeClient::ReaderLoop(std::uint64_t generation) {
    std::array<std::uint8_t, kReadBufBytes> buf{};
    WMCP_LOG(Trace, "ReaderLoop: entered");
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) {
        auto expected = generation;
        m_active_generation.compare_exchange_strong(expected, 0);
        m_connected.store(false);
        m_running.store(false);
        FailAllPending("could not create pipe reader event");
        return;
    }
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
                DWORD ignored = 0;
                ::GetOverlappedResult(m_pipe, &ov, &ignored, TRUE);
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
            if (!m_running.load()) break;
        }
        if (m_decoder.HasError()) {
            WMCP_LOG(Error, "decoder: frame too large; closing");
            break;
        }
    }
    if (ev) ::CloseHandle(ev);
    WMCP_LOG(Trace, "ReaderLoop: exiting");
    auto expected = generation;
    if (m_active_generation.compare_exchange_strong(expected, 0)) {
        m_connected.store(false);
    }
    m_running.store(false);
    m_identity_cv.notify_all();

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
    if (frame == "hello") {
        const auto protocol_version = msg.value("protocol_version", 0);
        const std::string instance_id =
            msg.value("bridge_instance_id", std::string{});
        if (protocol_version != windbgmcp::kProtocolVersion ||
            instance_id.empty()) {
            WMCP_LOG(Error,
                     "invalid bridge identity greeting; refusing unpinned transport");
        } else {
            std::lock_guard<std::mutex> identity_lk(m_identity_mu);
            if (m_observed_instance_id.empty()) {
                m_observed_instance_id = instance_id;
            } else if (m_observed_instance_id != instance_id) {
                WMCP_LOG(Error,
                         "bridge identity changed within one connection generation");
                m_observed_instance_id.clear();
            }
        }
        m_identity_cv.notify_all();
        return;
    }
    if (frame == "resp") {
        const auto id = msg.value("id", static_cast<std::int64_t>(0));
        if (msg.value("terminal", false) && !AcknowledgeTerminal(id)) {
            WMCP_LOG(Error,
                     "terminal response ACK failed; retiring ambiguous connection");
            m_running.store(false);
            m_connected.store(false);
            if (m_stop_evt) ::SetEvent(m_stop_evt);
            if (m_pipe != INVALID_HANDLE_VALUE) {
                ::CancelIoEx(m_pipe, nullptr);
            }
            return;
        }
        std::shared_ptr<Pending> slot;
        {
            std::lock_guard<std::mutex> lk(m_pending_mu);
            auto it = m_pending.find(id);
            if (it != m_pending.end()) {
                slot = it->second;
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
        std::shared_ptr<Pending> slot;
        {
            std::lock_guard<std::mutex> lk(m_pending_mu);
            auto it = m_pending.find(id);
            if (it == m_pending.end()) return;
            slot = it->second;
            if (!slot->sink) {
                slot->chunks.push_back(chunk);
                return;
            }
        }
        // User code may close/re-enter PipeClient. Keep the Pending alive via
        // shared_ptr, but never invoke callbacks while holding m_pending_mu.
        slot->sink(chunk, eof);
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

bool PipeClient::AcknowledgeTerminal(std::int64_t request_id) {
    const json ack = {
        {"frame", "ack"},
        {"terminal", true},
        {"id", request_id},
    };
    std::vector<std::uint8_t> wire;
    if (!Encode(ack.dump(), wire)) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(1'000);
    std::unique_lock<std::timed_mutex> write_lk(m_write_mu,
                                                std::defer_lock);
    if (!write_lk.try_lock_until(deadline) ||
        m_pipe == INVALID_HANDLE_VALUE || !m_connected.load()) {
        return false;
    }
    return WriteAllBounded(m_pipe, wire.data(),
                           static_cast<DWORD>(wire.size()),
                           m_stop_evt, deadline);
}

bool PipeClient::EnsurePinnedIdentity(
    std::uint64_t generation,
    std::chrono::steady_clock::time_point deadline,
    std::string& error) {
    std::unique_lock<std::mutex> lk(m_identity_mu);
    if (!m_identity_cv.wait_until(lk, deadline, [this, generation] {
            return !m_observed_instance_id.empty() ||
                   m_active_generation.load() != generation ||
                   !m_connected.load();
        })) {
        error = "bridge identity greeting timed out";
        return false;
    }
    if (m_active_generation.load() != generation || !m_connected.load()) {
        error = "connection closed before bridge identity was verified";
        return false;
    }
    if (m_observed_instance_id.empty()) {
        error = "extension did not provide a valid bridge identity";
        return false;
    }
    if (m_pinned_instance_id.empty()) {
        m_pinned_instance_id = m_observed_instance_id;
        WMCP_LOG(Info, "pinned bridge instance " + m_pinned_instance_id);
        return true;
    }
    if (m_pinned_instance_id != m_observed_instance_id) {
        error = "endpoint now belongs to a different bridge instance; "
                "restart windbg-mcp.exe to explicitly bind the new instance";
        WMCP_LOG(Error, error);
        return false;
    }
    return true;
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
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    if (!EnsureConnected(RemainingMs(deadline))) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return Response{false, {}, Error{"timeout",
                "request deadline elapsed while connecting to " + m_endpoint,
                "check that the extension is listening or raise timeout", 0}};
        }
        return Response{false, {}, Error{"disconnected",
            "could not open " + m_endpoint,
            "in WinDbg run: .load mcpext; !mcpext.start " + m_endpoint, 0}};
    }
    const auto generation = m_active_generation.load();
    if (generation == 0) {
        return Response{false, {}, Error{"disconnected",
            "connection closed before request could start", "retry the call", 0}};
    }
    std::string identity_error;
    if (!EnsurePinnedIdentity(generation, deadline, identity_error)) {
        {
            std::lock_guard<std::mutex> conn_lk(m_conn_mu);
            if (m_active_generation.load() == generation) {
                ResetConnectionLocked();
            }
        }
        const bool expired = std::chrono::steady_clock::now() >= deadline;
        return Response{false, {}, Error{
            expired ? "timeout" : "disconnected",
            identity_error,
            "verify the configured endpoint; restart windbg-mcp.exe to bind "
            "a deliberately restarted bridge instance",
            0}};
    }
    const auto id = m_next_id.fetch_add(1);
    auto slot = std::make_shared<Pending>();
    auto fut = slot->promise.get_future();
    slot->sink = std::move(chunk_sink);
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending[id] = slot;
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
    bool write_failed = false;
    DWORD write_error = ERROR_SUCCESS;
    {
        std::unique_lock<std::timed_mutex> lk(m_write_mu, std::defer_lock);
        if (!lk.try_lock_until(deadline)) {
            std::lock_guard<std::mutex> p(m_pending_mu);
            m_pending.erase(id);
            return Response{false, {}, Error{"timeout",
                "request deadline elapsed waiting for the pipe writer",
                "retry the call or raise timeout", 0}};
        }
        if (m_pipe == INVALID_HANDLE_VALUE ||
            m_active_generation.load() != generation ||
            !m_connected.load()) {
            std::lock_guard<std::mutex> p(m_pending_mu);
            m_pending.erase(id);
            return Response{false, {}, Error{"disconnected", "pipe closed", "", 0}};
        }
        const bool write_ok = WriteAllBounded(
            m_pipe, wire.data(), static_cast<DWORD>(wire.size()),
            m_stop_evt, deadline);
        write_error = write_ok ? ERROR_SUCCESS : ::GetLastError();
        write_failed = !write_ok;
        WMCP_LOG(Trace, "Request[" + std::to_string(id) +
                        "] bounded WriteFile returned ok=" +
                        std::to_string(write_ok) + " gle=" +
                        std::to_string(write_error));
    }
    if (write_failed) {
        {
            std::lock_guard<std::mutex> p(m_pending_mu);
            m_pending.erase(id);
        }
        // A failed or ambiguous byte-stream write makes this generation
        // unsafe for later frames. Retire it before another call reconnects.
        {
            std::lock_guard<std::mutex> conn_lk(m_conn_mu);
            if (m_active_generation.load() == generation) {
                ResetConnectionLocked();
            }
        }
        const bool expired = write_error == ERROR_TIMEOUT ||
                             std::chrono::steady_clock::now() >= deadline;
        return Response{false, {}, Error{
            expired ? "timeout" : "disconnected",
            expired ? "request deadline elapsed while writing to the extension"
                    : "pipe write failed or was cancelled",
            expired ? "raise timeout or check if WinDbg is responsive"
                    : "retry the call after the extension reconnects",
            0}};
    }
    WMCP_LOG(Trace, "Request[" + std::to_string(id) + "] waiting for future, timeout_ms=" +
                    std::to_string(timeout_ms));
    if (fut.wait_until(deadline) ==
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
