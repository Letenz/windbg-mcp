// SPDX-License-Identifier: MIT
//
// run_cmd: execute validated top-level statements with Unicode I/O and retain
// per-command evidence, including errors and deadlines.
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
#include "handlers/command_guard.h"

#include "ipc/pipe_server.h"
#include "util/debug_client.h"
#include "util/encoding.h"
#include "util/log.h"
#include "util/status.h"
#include "windbgmcp/protocol.h"

#include <DbgEng.h>
#include <atlbase.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

namespace {

class CaptureSink : public IDebugOutputCallbacksWide {
public:
    STDMETHOD(QueryInterface)(REFIID iid, PVOID* iface) override {
        if (!iface) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDebugOutputCallbacksWide)) {
            *iface = static_cast<IDebugOutputCallbacksWide*>(this);
            return S_OK;
        }
        *iface = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  override { return 1; } // stack-allocated, no refcount
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(Output)(ULONG mask, PCWSTR text) override {
        if (!text) return S_OK;
        // Filter to "interesting" output. NORMAL/ERROR/WARNING/SYMBOL covers
        // virtually everything an interactive user would see.
        const ULONG kept = DEBUG_OUTPUT_NORMAL | DEBUG_OUTPUT_ERROR |
                           DEBUG_OUTPUT_WARNING | DEBUG_OUTPUT_SYMBOLS |
                           DEBUG_OUTPUT_VERBOSE | DEBUG_OUTPUT_PROMPT_REGISTERS;
        if ((mask & kept) == 0) return S_OK;
        std::lock_guard<std::mutex> lk(m_mu);
        m_buf.append(text);
        if (mask & DEBUG_OUTPUT_ERROR) m_error = true;
        return S_OK;
    }

    std::pair<std::string, bool> Take() {
        std::lock_guard<std::mutex> lk(m_mu);
        std::wstring out;
        out.swap(m_buf);
        const bool error = m_error;
        m_error = false;
        return {encoding::WideToUtf8(out), error};
    }

private:
    std::mutex  m_mu;
    std::wstring m_buf;
    bool m_error = false;
};

struct CaptureBinding {
    IDebugClient5* client;
    ~CaptureBinding() { client->SetOutputCallbacksWide(nullptr); }
};

// Compose head + tail with truncation marker.
std::string HeadTailTruncate(const std::string& s,
                             std::size_t head_bytes,
                             std::size_t tail_bytes) {
    if (s.size() <= head_bytes + tail_bytes) return s;
    std::size_t head_end = head_bytes;
    std::size_t tail_begin = s.size() - tail_bytes;
    auto continuation = [&](std::size_t i) { return (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80; };
    while (head_end > 0 && continuation(head_end)) --head_end;
    while (tail_begin < s.size() && continuation(tail_begin)) ++tail_begin;
    const std::size_t hidden = tail_begin - head_end;
    char marker[96];
    std::snprintf(marker, sizeof(marker),
                  "\n[...truncated %zu bytes...]\n", hidden);
    std::string out;
    out.reserve(head_bytes + tail_bytes + 64);
    out.append(s, 0, head_end);
    out.append(marker);
    out.append(s, tail_begin, s.size() - tail_begin);
    return out;
}

// Send a chunk frame.
void SendChunk(ipc::PipeServer& pipe, std::int64_t req_id, std::uint32_t seq,
               bool eof, std::string_view chunk,
               ipc::ConnectionGeneration generation) {
    json frame = {
        {"frame", "chunk"},
        {"id",    req_id},
        {"seq",   seq},
        {"eof",   eof},
        {"chunk", chunk},
    };
    pipe.Send(frame.dump(), generation);
}

void StreamToFile(ipc::PipeServer& pipe, std::int64_t req_id,
                  const std::string& body, const std::string& path,
                  ipc::ConnectionGeneration generation) {
    // The extension owns the same-host file; chunks remain available to
    // protocol clients but must not trigger a second truncating file writer.
    {
        std::ofstream f(encoding::Utf8ToWide(path), std::ios::binary | std::ios::trunc);
        if (!f) {
            throw HandlerError(err::kInvalidArg, "cannot open output_file: " + path,
                               "check path is absolute and parent exists");
        }
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.close();
        if (!f) throw HandlerError(err::kEngineError, "cannot write complete output_file: " + path,
                                   "inspect retained output and available disk space; do not blindly repeat commands");
    }

    // Chunk the body. Roughly 64 KiB per chunk.
    constexpr std::size_t kChunkBytes = 64 * 1024;
    std::uint32_t seq = 0;
    for (std::size_t off = 0; off < body.size();) {
        std::size_t end = std::min(off + kChunkBytes, body.size());
        while (end < body.size() && (static_cast<unsigned char>(body[end]) & 0xc0) == 0x80) --end;
        const bool eof = end == body.size();
        SendChunk(pipe, req_id, seq++, eof,
                  std::string_view(body.data() + off, end - off), generation);
        off = end;
    }
    if (body.empty()) {
        SendChunk(pipe, req_id, 0, true, "", generation);
    }
}

} // namespace

json RunCmd(std::int64_t req_id, const json& args, ipc::PipeServer& pipe,
            ipc::ConnectionGeneration generation) {
    if (!args.contains("cmd") || !args["cmd"].is_string()) {
        throw HandlerError(err::kInvalidArg, "cmd is required and must be a string", "");
    }
    const std::string cmd = args["cmd"].get<std::string>();
    const auto batch = ParseCommandBatch(cmd);
    if (!batch.error.empty()) throw HandlerError(err::kInvalidArg, batch.error, "fix the command delimiters");
    if (const auto unsafe = UnsafeRunControl(batch)) {
        throw HandlerError(err::kInvalidArg, *unsafe, "put g/step in a separate call or as the final statement");
    }
    if (const auto unsafe = UnsafeLifecycleCommand(cmd)) {
        throw HandlerError(
            err::kInvalidArg,
            "lifecycle-changing command refused on the MCP worker: " + *unsafe,
            "use wm_shutdown/wm_detach or the debugger's own command window for lifecycle changes");
    }
    const std::uint32_t timeout_ms =
        args.value("timeout_ms", static_cast<std::uint32_t>(kTimeoutStandardMs));
    const std::string output_file = args.value("output_file", std::string{});
    const std::uint32_t preview_head =
        args.value("preview_bytes", static_cast<std::uint32_t>(kDefaultPreviewHead));

    // Validate timeout.
    if (timeout_ms == 0) {
        throw HandlerError(err::kInvalidArg, "timeout_ms must be > 0", "");
    }
    const auto deadline = (std::min)(
        args.value("deadline_tick_ms", ::GetTickCount64() + timeout_ms),
        ::GetTickCount64() + timeout_ms);
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

    // Capture through a separate client without replacing the UI callbacks.
    CaptureSink sink;
    CComPtr<IDebugClient5> cap_client;
    HRESULT hr = ::DebugCreate(__uuidof(IDebugClient5), reinterpret_cast<void**>(&cap_client));
    if (FAILED(hr)) {
        throw HandlerError(err::kEngineError, "DebugCreate (capture) failed", "", hr);
    }
    hr = cap_client->SetOutputCallbacksWide(&sink);
    if (FAILED(hr)) throw HandlerError(err::kEngineError, "SetOutputCallbacksWide failed", "", hr);
    CaptureBinding binding{cap_client};

    // Use the capture client's IDebugControl so the output goes to OUR sink
    // and not WinDbg's UI. dbgeng will fan out to all clients' callbacks
    // anyway, but the WinDbg UI sink is a separate one we don't replace.
    CComQIPtr<IDebugControl4> cap_ctl(cap_client);
    if (!cap_ctl) {
        throw HandlerError(err::kEngineError, "QI IDebugControl4 (capture) failed", "");
    }

    json response = {{"ok", true}, {"results", json::array()},
                     {"commands_total", batch.commands.size()}, {"commands_executed", 0},
                     {"execution_may_continue", false}, {"safe_to_retry", false}};
    std::string utf8;
    std::size_t executed = 0;
    bool stopped = false;
    for (std::size_t i = 0; i < batch.commands.size(); ++i) {
        const auto& command = batch.commands[i];
        if (stopped || ::GetTickCount64() >= deadline) {
            response["results"].push_back({{"index", i}, {"command", command}, {"status", "not_executed"}});
            if (!stopped) {
                response["ok"] = false;
                response["err"] = {{"code", "timeout"}, {"msg", "command budget expired before execution"},
                                   {"tip", "inspect results; do not repeat already executed commands"}};
                stopped = true;
            }
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        const auto wide_command = encoding::Utf8ToWide(command);
        hr = cap_ctl->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT | DEBUG_OUTCTL_OVERRIDE_MASK | DEBUG_OUTCTL_NOT_LOGGED,
                                 wide_command.c_str(), DEBUG_EXECUTE_NO_REPEAT | DEBUG_EXECUTE_NOT_LOGGED);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        auto [output, error_output] = sink.Take();
        utf8 += output;
        ++executed;
        const bool expired = ::GetTickCount64() >= deadline;
        const bool failed = FAILED(hr) || error_output;
        response["results"].push_back({
            {"index", i}, {"command", command},
            {"status", failed ? "failed" : expired ? "completed_after_deadline" : "succeeded"},
            {"hresult", static_cast<long>(hr)}, {"error_output", error_output}, {"elapsed_ms", elapsed},
            {"output", HeadTailTruncate(output, 1536, 512)},
            {"truncated_in_response", output.size() > 2048},
        });
        if (failed || expired) {
            stopped = true;
            response["ok"] = false;
            response["failed_command_index"] = i;
            response["err"] = {{"code", failed ? "engine_error" : "timeout"},
                               {"msg", failed ? "debugger command failed; partial output retained" : "command exceeded its budget; remaining commands skipped"},
                               {"tip", "inspect results/output before deciding what to run next; prior effects are not rolled back"},
                               {"hr", static_cast<long>(hr)}};
        }
    }
    response["commands_executed"] = executed;
    response["commands_skipped"] = batch.commands.size() - executed;
    const std::size_t total = utf8.size();

    ULONG exec_after = 0;
    ctl->GetExecutionStatus(&exec_after);

    // Streaming branch.
    if (!output_file.empty()) {
        response["output_file_written"] = false;
        try {
            StreamToFile(pipe, req_id, utf8, output_file, generation);
            response["output_file_written"] = true;
        } catch (const HandlerError& error) {
            response["ok"] = false;
            response["output_file_error"] = {{"code", error.code()}, {"msg", error.msg()}, {"tip", error.tip()}};
            if (!response.contains("err")) response["err"] = response["output_file_error"];
        }
        std::string preview = HeadTailTruncate(utf8, preview_head, kPreviewTail);
        response.update({{"output", preview}, {"output_file", output_file}, {"bytes_total", total},
                         {"truncated_in_response", total > preview_head + kPreviewTail},
                         {"exec_status_after", status::ToString(exec_after)}});
        return response;
    }

    // Non-streaming branch.
    if (utf8.size() > kInlineOutputSoftLimit) {
        std::string preview = HeadTailTruncate(utf8, preview_head, kPreviewTail);
        response.update({{"output", preview}, {"bytes_total", total}, {"truncated_in_response", true},
                         {"exec_status_after", status::ToString(exec_after)},
                         {"hint", "specify output_file to capture full output"}});
        return response;
    }

    response.update({{"output", utf8}, {"bytes_total", total}, {"truncated_in_response", false},
                     {"exec_status_after", status::ToString(exec_after)}});
    return response;
}

} // namespace windbgmcp::handlers
