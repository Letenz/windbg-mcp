// SPDX-License-Identifier: MIT
//
// wm_run_cmd: validated batches with a shared deadline and retained evidence.
// The same-host extension owns output_file and returns a UTF-8 preview.

#include "tools/dispatcher.h"
#include "util/path.h"

#include <Windows.h>

namespace wmh::tools {

using json = nlohmann::json;

Result RunCmd(transport::PipeClient& pipe, const json& args) {
    if (!args.contains("cmd") || !args["cmd"].is_string() ||
        args["cmd"].get<std::string>().empty()) {
        return ErrJson("invalid_arg", "cmd must be a non-empty string");
    }
    const std::string cmd = args["cmd"].get<std::string>();
    for (const auto& [key, value] : args.items()) {
        if (key != "cmd" && key != "timeout_ms" && key != "preview_bytes" && key != "output_file")
            return ErrJson("invalid_arg", "unknown run_cmd argument: " + key);
    }
    for (auto key : {"timeout_ms", "preview_bytes"}) {
        if (!args.contains(key)) continue;
        const auto& value = args[key];
        const auto minimum = std::string(key) == "timeout_ms" ? 1 : 0;
        const auto maximum = std::string(key) == "timeout_ms" ? 120000 : 1048576;
        if (!value.is_number_integer() || value < minimum || value > maximum)
            return ErrJson("invalid_arg", std::string(key) + " is outside its integer range");
    }
    const std::uint32_t timeout_ms = args.value("timeout_ms", 30000u);
    const std::uint32_t preview_bytes = args.value("preview_bytes", 8192u);

    json forward = {
        {"cmd",           cmd},
        {"timeout_ms",    timeout_ms},
        {"preview_bytes", preview_bytes},
        {"deadline_tick_ms", ::GetTickCount64() + timeout_ms},
    };

    if (args.contains("output_file")) {
        if (!args["output_file"].is_string()) return ErrJson("invalid_arg", "output_file must be a string");
        std::string err;
        auto v = path::ValidateOutputFile(args["output_file"].get<std::string>(), err);
        if (!v) return ErrJson("invalid_arg", err);
        forward["output_file"] = v->utf8;
    }
    // The extension is on the same host and owns the output file. Do not open
    // a second truncating writer before validation or overwrite error evidence.
    auto resp = pipe.Request("run_cmd", forward, timeout_ms + 5000);
    auto result = FromResponse(resp);
    if (!resp.ok && (resp.err.code == "timeout" || resp.err.code == "disconnected")) {
        auto payload = json::parse(result.text);
        payload["execution_may_continue"] = true;
        payload["safe_to_retry"] = false;
        payload["execution_state"] = "unknown";
        payload["err"]["tip"] = "do not resubmit blindly; wait for the debugger to respond and inspect state; a timeout is not cancellation or rollback";
        result.text = payload.dump();
    }
    return result;
}

} // namespace wmh::tools
