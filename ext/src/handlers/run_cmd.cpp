// SPDX-License-Identifier: MIT
//
// run_cmd: pipe a string to IDebugControl::Execute, capture all output via
// a temporary IDebugOutputCallbacks, return ANSI->UTF8 text.
//
// Truncation: caller can request output_file streaming via the chunk
// channel; if not, we return the full output up to kInlineOutputSoftLimit
// bytes. Above that we force head + tail truncation and tell the AI.
//
// Streaming detail: the chunk channel exists in the protocol, but for
// IDebugControl::Execute we cannot stream during the call (the API is
// synchronous and dbgeng doesn't return until the command completes). For
// commands that emit a lot of output, we instead buffer everything in
// memory and chunk it after Execute returns. This keeps the wire happy
// even when one frame would exceed kMaxFrameBytes.

#include "handlers/handlers.h"

#include "ipc/pipe_server.h"
#include "util/debug_client.h"
#include "util/encoding.h"
#include "util/log.h"
#include "util/status.h"
#include "windbgmcp/protocol.h"

#include <DbgEng.h>
#include <atlbase.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

namespace {

class CaptureSink : public IDebugOutputCallbacks {
public:
    STDMETHOD(QueryInterface)(REFIID iid, PVOID* iface) override {
        if (!iface) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDebugOutputCallbacks)) {
            *iface = static_cast<IDebugOutputCallbacks*>(this);
            return S_OK;
        }
        *iface = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  override { return 1; } // stack-allocated, no refcount
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(Output)(ULONG mask, PCSTR text) override {
        if (!text) return S_OK;
        // Filter to "interesting" output. NORMAL/ERROR/WARNING/SYMBOL covers
        // virtually everything an interactive user would see.
        const ULONG kept = DEBUG_OUTPUT_NORMAL | DEBUG_OUTPUT_ERROR |
                           DEBUG_OUTPUT_WARNING | DEBUG_OUTPUT_SYMBOLS |
                           DEBUG_OUTPUT_VERBOSE | DEBUG_OUTPUT_PROMPT_REGISTERS;
        if ((mask & kept) == 0) return S_OK;
        std::lock_guard<std::mutex> lk(m_mu);
        m_buf.append(text);
        return S_OK;
    }

    std::string Take() {
        std::lock_guard<std::mutex> lk(m_mu);
        std::string out;
        out.swap(m_buf);
        return out;
    }

private:
    std::mutex  m_mu;
    std::string m_buf;
};

// Compose head + tail with truncation marker.
std::string HeadTailTruncate(const std::string& s,
                             std::size_t head_bytes,
                             std::size_t tail_bytes) {
    if (s.size() <= head_bytes + tail_bytes) return s;
    const std::size_t hidden = s.size() - head_bytes - tail_bytes;
    char marker[96];
    std::snprintf(marker, sizeof(marker),
                  "\n[...truncated %zu bytes...]\n", hidden);
    std::string out;
    out.reserve(head_bytes + tail_bytes + 64);
    out.append(s, 0, head_bytes);
    out.append(marker);
    out.append(s, s.size() - tail_bytes, tail_bytes);
    return out;
}

// Send a chunk frame.
void SendChunk(ipc::PipeServer& pipe, std::int64_t req_id, std::uint32_t seq,
               bool eof, std::string_view chunk) {
    json frame = {
        {"frame", "chunk"},
        {"id",    req_id},
        {"seq",   seq},
        {"eof",   eof},
        {"chunk", chunk},
    };
    pipe.Send(frame.dump());
}

void StreamToFile(ipc::PipeServer& pipe, std::int64_t req_id,
                  const std::string& body, const std::string& path) {
    // Write to disk, then chunk to the wire. The protocol guarantees the
    // server applies file writes in chunk-arrival order; since we send
    // everything before returning, ordering is trivially correct.
    {
        std::ofstream f(encoding::Utf8ToWide(path), std::ios::binary | std::ios::trunc);
        if (!f) {
            throw HandlerError(err::kInvalidArg, "cannot open output_file: " + path,
                               "check path is absolute and parent exists");
        }
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    // Chunk the body. Roughly 64 KiB per chunk.
    constexpr std::size_t kChunkBytes = 64 * 1024;
    std::uint32_t seq = 0;
    for (std::size_t off = 0; off < body.size(); off += kChunkBytes) {
        const std::size_t take = std::min(kChunkBytes, body.size() - off);
        const bool eof = (off + take) == body.size();
        SendChunk(pipe, req_id, seq++, eof,
                  std::string_view(body.data() + off, take));
    }
    if (body.empty()) {
        SendChunk(pipe, req_id, 0, true, "");
    }
}

} // namespace

json RunCmd(std::int64_t req_id, const json& args, ipc::PipeServer& pipe) {
    if (!args.contains("cmd") || !args["cmd"].is_string()) {
        throw HandlerError(err::kInvalidArg, "cmd is required and must be a string", "");
    }
    const std::string cmd = args["cmd"].get<std::string>();
    const std::uint32_t timeout_ms =
        args.value("timeout_ms", static_cast<std::uint32_t>(kTimeoutStandardMs));
    const std::string output_file = args.value("output_file", std::string{});
    const std::uint32_t preview_head =
        args.value("preview_bytes", static_cast<std::uint32_t>(kDefaultPreviewHead));

    // Validate timeout.
    if (timeout_ms == 0) {
        throw HandlerError(err::kInvalidArg, "timeout_ms must be > 0", "");
    }
    // Validate output_file path if given.
    if (!output_file.empty()) {
        if (output_file.find("..") != std::string::npos) {
            throw HandlerError(err::kInvalidArg, "output_file may not contain '..'", "");
        }
        // Require a drive letter or leading slash.
        if (output_file.size() < 2 ||
            !(std::isalpha(static_cast<unsigned char>(output_file[0])) && output_file[1] == ':')) {
            throw HandlerError(err::kInvalidArg, "output_file must be an absolute path",
                               "use F:/path/file.txt or C:\\\\path\\\\file.txt");
        }
    }

    auto client = dbg::Get();
    if (!client) {
        throw HandlerError(err::kEngineError, "no IDebugClient", "");
    }
    CComQIPtr<IDebugControl4> ctl(client);
    if (!ctl) {
        throw HandlerError(err::kEngineError, "QI IDebugControl4 failed", "");
    }

    // Serialise the entire dbgeng interaction. Execute, attached-check,
    // execution-status query, and the capture client's output callback all
    // share the KD transport on a kernel target. Concurrent handlers must
    // queue or dbgeng will surface "Kernel transport in use".
    std::lock_guard<std::mutex> engine_lk(dbg::Lock());

    // Refuse if not attached.
    ULONG cls = 0, qual = 0;
    ctl->GetDebuggeeType(&cls, &qual);
    if (cls == DEBUG_CLASS_UNINITIALIZED) {
        throw HandlerError(err::kNotAttached, "no debug target attached", "open a dump or wait for KD");
    }

    // Refuse if running. Most commands need BREAK; we draw the line here
    // rather than parsing the cmd.
    ULONG exec_before = 0;
    ctl->GetExecutionStatus(&exec_before);
    if (status::IsRunning(exec_before)) {
        throw HandlerError(err::kTargetRunning,
                           "target is running; most commands require BREAK",
                           "call wm_break_in first");
    }

    // Install our output capture, swapping out (and restoring) any existing one.
    CComPtr<IDebugClient> cap_client;
    HRESULT hr = ::DebugCreate(__uuidof(IDebugClient), reinterpret_cast<void**>(&cap_client));
    if (FAILED(hr)) {
        throw HandlerError(err::kEngineError, "DebugCreate (capture) failed", "", hr);
    }
    CaptureSink sink;
    cap_client->SetOutputCallbacks(&sink);

    // Use the capture client's IDebugControl so the output goes to OUR sink
    // and not WinDbg's UI. dbgeng will fan out to all clients' callbacks
    // anyway, but the WinDbg UI sink is a separate one we don't replace.
    CComQIPtr<IDebugControl4> cap_ctl(cap_client);
    if (!cap_ctl) {
        throw HandlerError(err::kEngineError, "QI IDebugControl4 (capture) failed", "");
    }

    WMCP_LOG(Info, std::string("run_cmd[") + std::to_string(req_id) +
                   "] cmd=" + cmd);
    const auto t_exec_start = std::chrono::steady_clock::now();
    hr = cap_ctl->Execute(DEBUG_OUTCTL_THIS_CLIENT |
                          DEBUG_OUTCTL_OVERRIDE_MASK |
                          DEBUG_OUTCTL_NOT_LOGGED,
                          cmd.c_str(),
                          DEBUG_EXECUTE_NO_REPEAT | DEBUG_EXECUTE_NOT_LOGGED);
    const auto t_exec_end = std::chrono::steady_clock::now();
    const auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_exec_end - t_exec_start).count();
    cap_client->SetOutputCallbacks(nullptr);
    WMCP_LOG(Info, std::string("run_cmd[") + std::to_string(req_id) +
                   "] Execute returned hr=0x" +
                   [hr]{ char b[16]; std::snprintf(b, sizeof(b), "%08x",
                                          static_cast<unsigned>(hr)); return std::string(b); }() +
                   " elapsed_ms=" + std::to_string(exec_ms));

    if (FAILED(hr)) {
        throw HandlerError(err::kEngineError,
                           "IDebugControl::Execute failed",
                           "check command syntax", hr);
    }

    // Convert ANSI output to UTF-8 once.
    std::string captured = sink.Take();
    std::string utf8 = encoding::AcpToUtf8(captured);
    const std::size_t total = utf8.size();

    ULONG exec_after = 0;
    ctl->GetExecutionStatus(&exec_after);

    // Streaming branch.
    if (!output_file.empty()) {
        StreamToFile(pipe, req_id, utf8, output_file);
        std::string preview = HeadTailTruncate(utf8, preview_head, kPreviewTail);
        return json{
            {"output",                preview},
            {"output_file",           output_file},
            {"bytes_total",           total},
            {"truncated_in_response", utf8.size() > preview.size()},
            {"exec_status_after",     status::ToString(exec_after)},
        };
    }

    // Non-streaming branch.
    if (utf8.size() > kInlineOutputSoftLimit) {
        std::string preview = HeadTailTruncate(utf8, preview_head, kPreviewTail);
        return json{
            {"output",                preview},
            {"bytes_total",           total},
            {"truncated_in_response", true},
            {"exec_status_after",     status::ToString(exec_after)},
            {"hint",                  "specify output_file to capture full output"},
        };
    }

    return json{
        {"output",                utf8},
        {"bytes_total",           total},
        {"truncated_in_response", false},
        {"exec_status_after",     status::ToString(exec_after)},
    };
}

} // namespace windbgmcp::handlers
