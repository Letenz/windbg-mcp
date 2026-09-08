// SPDX-License-Identifier: MIT
//
// wm_analyze_crash: server-side composition of !analyze -v + kb + lm +
// !drvobj, with structured parsing on top. Same shape as the Python
// prototype, ported to C++.

#include "tools/dispatcher.h"

#include "analysis/bugcheck_parser.h"
#include "analysis/stack_parser.h"
#include "util/path.h"

#include <fstream>
#include <string>

namespace wmh::tools {

using json = nlohmann::json;

namespace {

// Helper: run a single command, return its output text (empty on failure).
// `out_err` receives a structured error if the call fails; caller decides
// whether to abort.
std::string RunOnce(transport::PipeClient& pipe, const std::string& cmd,
                    std::uint32_t timeout_ms, json* out_err) {
    auto resp = pipe.Request("run_cmd",
                             json{{"cmd", cmd}, {"timeout_ms", timeout_ms}},
                             timeout_ms + 5000);
    if (!resp.ok) {
        if (out_err) {
            *out_err = json{
                {"code", resp.err.code},
                {"msg",  resp.err.msg},
                {"tip",  resp.err.tip},
            };
        }
        return {};
    }
    if (out_err && resp.data.value("ok", true) == false)
        *out_err = resp.data.value("err", json::object());
    return resp.data.value("output", std::string{});
}

std::string StripDotSys(const std::string& s) {
    constexpr const char* kSuffix = ".sys";
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, kSuffix) == 0) {
        return s.substr(0, s.size() - 4);
    }
    return s;
}

} // namespace

Result AnalyzeCrash(transport::PipeClient& pipe, const json& args) {
    // Verify session state first.
    auto sess_resp = pipe.Request("session", json::object(), 5000);
    if (!sess_resp.ok) return FromResponse(sess_resp);
    if (!sess_resp.data.value("attached", false)) {
        return ErrJson("not_attached", "no debug target attached",
                       "open a dump or wait for KD");
    }
    if (sess_resp.data.value("is_running", false)) {
        return ErrJson("target_running",
                       "target is running; bugcheck analysis requires the target halted",
                       "call wm_break_in first or wait for the bugcheck event");
    }

    json fatal = nullptr;
    const std::string analyze_text = RunOnce(pipe, "!analyze -v", 120000, &fatal);
    if (analyze_text.empty() && !fatal.is_null()) {
        return ErrJson(fatal.value("code", "engine_error"),
                       fatal.value("msg", "!analyze -v failed"),
                       fatal.value("tip", ""));
    }

    const std::string kb_text = RunOnce(pipe, "kb", 30000, nullptr);

    json bugcheck = analysis::bugcheck::ParseBugcheck(analyze_text);
    json faulting = analysis::bugcheck::ParseFaulting(analyze_text);
    json culprit  = analysis::bugcheck::ParseProbableCulprit(analyze_text);

    std::string target_mod;
    if (faulting.is_object() && faulting.contains("module") && faulting["module"].is_string()) {
        target_mod = faulting["module"].get<std::string>();
    } else if (culprit.is_object() && culprit.contains("module") && culprit["module"].is_string()) {
        target_mod = culprit["module"].get<std::string>();
    }

    std::string lm_text;
    std::string drvobj_text;
    if (!target_mod.empty()) {
        std::string mod = StripDotSys(target_mod);
        lm_text = RunOnce(pipe, "lm m " + mod, 15000, nullptr);
        if (sess_resp.data.value("target_kind", std::string{}) == "kernel") {
            drvobj_text = RunOnce(pipe, "!drvobj " + mod + " 2", 15000, nullptr);
        }
    }

    json stack   = analysis::stack::ParseKb(analyze_text);
    if (stack.empty()) stack = analysis::stack::ParseKb(kb_text);
    json iret    = analysis::bugcheck::ParseIretFrame(analyze_text);
    json context = analysis::bugcheck::ParseContext(analyze_text);

    json report = {
        {"bugcheck",         bugcheck},
        {"faulting",         faulting},
        {"probable_culprit", culprit},
        {"stack",            stack},
        {"iret_frame",       iret},
        {"context",          context},
        {"raw", {
            {"analyze", analyze_text},
            {"kb",      kb_text},
            {"lm",      lm_text},
            {"drvobj",  drvobj_text.empty() ? json(nullptr) : json(drvobj_text)},
        }},
    };

    if (args.contains("output_file") && args["output_file"].is_string()) {
        std::string err;
        auto v = path::ValidateOutputFile(args["output_file"].get<std::string>(), err);
        if (!v) return ErrJson("invalid_arg", err);
        std::ofstream f(v->wide, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            return ErrJson("invalid_arg",
                           "cannot open output_file: " + v->utf8);
        }
        f << "===== !analyze -v =====\n" << analyze_text;
        f << "\n===== kb =====\n"        << kb_text;
        if (!lm_text.empty()) {
            f << "\n===== lm m " << target_mod << " =====\n" << lm_text;
        }
        if (!drvobj_text.empty()) {
            f << "\n===== !drvobj " << target_mod << " 2 =====\n" << drvobj_text;
        }
        report["output_file"] = v->utf8;
    }

    return OkJson(report);
}

} // namespace wmh::tools
