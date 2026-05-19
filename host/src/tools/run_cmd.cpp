// SPDX-License-Identifier: MIT
//
// wm_run_cmd: passthrough + optional streaming to a local file.
//
// When the AI supplies `output_file`, we open the file on the host side
// and the chunk_sink callback writes UTF-8 bytes as they arrive on the
// pipe. The final response payload still contains a head+tail preview so
// the AI can reason about content without re-reading the file.

#include "tools/dispatcher.h"
#include "util/path.h"

#include <fstream>
#include <memory>
#include <mutex>

namespace wmh::tools {

using json = nlohmann::json;

Result RunCmd(transport::PipeClient& pipe, const json& args) {
    if (!args.contains("cmd") || !args["cmd"].is_string() ||
        args["cmd"].get<std::string>().empty()) {
        return ErrJson("invalid_arg", "cmd must be a non-empty string");
    }
    const std::string cmd = args["cmd"].get<std::string>();
    const std::uint32_t timeout_ms = args.value("timeout_ms", 30000u);
    if (timeout_ms == 0) {
        return ErrJson("invalid_arg", "timeout_ms must be > 0");
    }
    const std::uint32_t preview_bytes = args.value("preview_bytes", 8192u);

    json forward = {
        {"cmd",           cmd},
        {"timeout_ms",    timeout_ms},
        {"preview_bytes", preview_bytes},
    };

    std::shared_ptr<std::ofstream> file_out;
    std::shared_ptr<std::mutex>    file_mu;

    if (args.contains("output_file") && args["output_file"].is_string()) {
        std::string err;
        auto v = path::ValidateOutputFile(args["output_file"].get<std::string>(), err);
        if (!v) return ErrJson("invalid_arg", err);
        file_out = std::make_shared<std::ofstream>(v->wide,
                       std::ios::binary | std::ios::trunc);
        if (!file_out->is_open()) {
            return ErrJson("invalid_arg",
                           "cannot open output_file: " + v->utf8,
                           "check that the parent directory exists");
        }
        file_mu = std::make_shared<std::mutex>();
        forward["output_file"] = v->utf8;
    }

    transport::ChunkSink sink;
    if (file_out) {
        sink = [file_out, file_mu](std::string_view chunk, bool eof) {
            std::lock_guard<std::mutex> lk(*file_mu);
            if (!chunk.empty()) {
                file_out->write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            }
            if (eof) file_out->close();
        };
    }

    auto resp = pipe.Request("run_cmd", forward, timeout_ms + 5000, sink);
    return FromResponse(resp);
}

} // namespace wmh::tools
