// SPDX-License-Identifier: MIT
#include "tools/dispatcher.h"

#include <chrono>

namespace wmh::tools {

using json = nlohmann::json;

// Forward declarations from sibling .cpp files.
Result RunCmd       (transport::PipeClient& pipe, const json& args);
Result AnalyzeCrash (transport::PipeClient& pipe, const json& args);

Dispatcher::Dispatcher(transport::PipeClient& pipe) : m_pipe(pipe) {}

Result OkJson(const json& body) {
    Result r;
    r.text = body.dump();
    return r;
}
Result ErrJson(const std::string& code, const std::string& msg,
               const std::string& tip, long hr) {
    json e = {{"ok", false}, {"err", {{"code", code}, {"msg", msg}, {"tip", tip}}}};
    if (hr) e["err"]["hr"] = hr;
    Result r;
    r.text = e.dump();
    r.is_error = true;
    return r;
}
Result FromResponse(const transport::Response& resp) {
    if (resp.ok) {
        auto result = OkJson(resp.data);
        if (resp.data.is_object() && resp.data.value("ok", true) == false) result.is_error = true;
        return result;
    }
    return ErrJson(resp.err.code, resp.err.msg, resp.err.tip, resp.err.hr);
}

json Dispatcher::Descriptors() const {
    auto tool = [](const char* name, const char* desc,
                   const json& schema) -> json {
        return json{
            {"name",        name},
            {"description", desc},
            {"inputSchema", schema},
        };
    };

    json empty = {
        {"type", "object"},
        {"properties", json::object()},
        {"additionalProperties", false},
    };
    json run_cmd_schema = {
        {"type", "object"},
        {"properties", {
            {"cmd",           {{"type", "string"},  {"description", "UTF-8 commands separated by semicolons or top-level newlines. Stops on first error. Run-control must be the final standalone statement."}}},
            {"timeout_ms",    {{"type", "integer"}, {"minimum", 1}, {"maximum", 120000}, {"default", 30000}}},
            {"output_file",   {{"type", "string"},  {"description", "Absolute path to stream large output"}}},
            {"preview_bytes", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1048576}, {"default", 8192}}},
        }},
        {"required", json::array({"cmd"})},
        {"additionalProperties", false},
    };
    json wait_event_schema = {
        {"type", "object"},
        {"properties", {
            {"kinds",      {{"type", "array"}, {"items", {{"type", "string"}}},
                            {"description", "bugcheck / break / breakpoint_hit / module_load / module_unload / session_status / state_change"}}},
            {"timeout_ms", {{"type", "integer"}, {"default", 60000}}},
            {"since_ms",   {{"type", "integer"}, {"description", "Unix ms; replay matching events newer than this from the last 30s"}}},
        }},
        {"required", json::array({"kinds"})},
        {"additionalProperties", false},
    };
    json break_in_schema = {
        {"type", "object"},
        {"properties", {
            {"timeout_ms", {{"type", "integer"}, {"default", 30000}}},
        }},
        {"additionalProperties", false},
    };
    json analyze_schema = {
        {"type", "object"},
        {"properties", {
            {"output_file", {{"type", "string"}, {"description", "Absolute path; write concatenated raw output"}}},
        }},
        {"additionalProperties", false},
    };

    return json::array({
        tool("wm_session",       "Snapshot of debugger state. Cheap; use as a liveness probe.", empty),
        tool("wm_run_cmd",       "Run debugger statements sequentially with per-command results and partial output on failure. Use ; or top-level newlines. Timeout is not rollback; never blindly retry an uncertain execution.", run_cmd_schema),
        tool("wm_wait_event",    "Block until a debugger event arrives. Pass since_ms to replay the last 30s of matching events.", wait_event_schema),
        tool("wm_break_in",      "Halt a running target via SetInterrupt; waits on the ext's break event.", break_in_schema),
        tool("wm_analyze_crash", "Structured BSOD report: !analyze -v + kb + lm + !drvobj, parsed.", analyze_schema),
        tool("wm_detach",        "Detach the debugger target only; keep the bridge and host running.", empty),
        tool("wm_shutdown",      "Stop the WinDbg bridge after delivering the response; keep the target attached and MCP host running.", empty),
        tool("wm_exit",          "Deprecated alias for wm_detach; it does not stop the bridge or WinDbg.", empty),
    });
}

Result Dispatcher::Call(const std::string& name, const json& args) {
    if (name == "wm_session")        return CallSession(args);
    if (name == "wm_run_cmd")        return RunCmd(m_pipe, args);
    if (name == "wm_wait_event")     return CallWaitEvent(args);
    if (name == "wm_break_in")       return CallBreakIn(args);
    if (name == "wm_analyze_crash")  return AnalyzeCrash(m_pipe, args);
    if (name == "wm_detach")         return CallDetach(args);
    if (name == "wm_shutdown")       return CallShutdown(args);
    if (name == "wm_exit")           return CallExit(args);
    return ErrJson("invalid_arg", "unknown tool: " + name, "see tools/list");
}

Result Dispatcher::CallSession(const json& /*args*/) {
    auto resp = m_pipe.Request("session", json::object(), 5000);
    return FromResponse(resp);
}

Result Dispatcher::CallWaitEvent(const json& args) {
    if (!args.contains("kinds") || !args["kinds"].is_array() || args["kinds"].empty()) {
        return ErrJson("invalid_arg", "kinds must be a non-empty array");
    }
    int timeout_ms = args.value("timeout_ms", 60000);
    if (timeout_ms <= 0) return ErrJson("invalid_arg", "timeout_ms must be > 0");

    json forward = {{"kinds", args["kinds"]}, {"timeout_ms", timeout_ms}};
    // Default: replay the last 10 s of matching events. The AI typically
    // calls wait_event AFTER triggering the action that causes the event
    // (e.g. issuing `g` then wm_wait_event for the break) — the event has
    // almost always already fired. Without a lookback the call waits the
    // full timeout for an event that already passed. Callers can still
    // pass since_ms=0 explicitly to suppress the replay.
    if (args.contains("since_ms") && args["since_ms"].is_number_integer()) {
        forward["since_ms"] = args["since_ms"];
    } else {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t since = (now_ms > 10'000) ? (now_ms - 10'000) : 0;
        forward["since_ms"] = since;
    }

    // Give the ext a small grace window beyond its own deadline so it
    // gets a chance to time out first and produce a structured error.
    auto resp = m_pipe.Request("wait_event", forward,
                               static_cast<std::uint32_t>(timeout_ms) + 5000);
    return FromResponse(resp);
}

Result Dispatcher::CallBreakIn(const json& args) {
    int timeout_ms = args.value("timeout_ms", 30000);
    if (timeout_ms <= 0) return ErrJson("invalid_arg", "timeout_ms must be > 0");
    auto resp = m_pipe.Request("break_in", {{"timeout_ms", timeout_ms}},
                               static_cast<std::uint32_t>(timeout_ms) + 5000);
    return FromResponse(resp);
}

Result Dispatcher::CallDetach(const json& /*args*/) {
    auto resp = m_pipe.Request("detach", json::object(), 5000);
    return FromResponse(resp);
}

Result Dispatcher::CallShutdown(const json& /*args*/) {
    auto resp = m_pipe.Request("shutdown", json::object(), 5000);
    return FromResponse(resp);
}

Result Dispatcher::CallExit(const json& /*args*/) {
    auto resp = m_pipe.Request("exit", json::object(), 5000);
    return FromResponse(resp);
}

} // namespace wmh::tools
