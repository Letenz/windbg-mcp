// SPDX-License-Identifier: MIT
//
// wait_event handler: blocks the worker thread on the publisher.

#include "handlers/handlers.h"

#include "events/publisher.h"
#include "windbgmcp/protocol.h"

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

json WaitEvent(std::int64_t /*req_id*/, const json& args, ipc::PipeServer& /*pipe*/,
               ipc::ConnectionGeneration /*generation*/) {
    if (!args.contains("kinds") || !args["kinds"].is_array()) {
        throw HandlerError(err::kInvalidArg, "kinds must be a non-empty array of strings", "");
    }
    std::vector<std::string> kinds;
    for (const auto& k : args["kinds"]) {
        if (!k.is_string()) {
            throw HandlerError(err::kInvalidArg, "kinds[*] must be strings", "");
        }
        kinds.push_back(k.get<std::string>());
    }
    if (kinds.empty()) {
        throw HandlerError(err::kInvalidArg, "kinds is empty", "");
    }
    if (!args.contains("timeout_ms") || !args["timeout_ms"].is_number_integer()) {
        throw HandlerError(err::kInvalidArg, "timeout_ms is required (int)", "");
    }
    const std::uint32_t timeout_ms = args["timeout_ms"].get<std::uint32_t>();
    const std::uint64_t since_ms   = args.value("since_ms", static_cast<std::uint64_t>(0));

    auto ev = events::Publisher::Get().Wait(kinds, timeout_ms, since_ms);
    if (!ev) {
        throw HandlerError(err::kTimeout, "no matching event in window", "raise timeout_ms");
    }
    return json{
        {"kind", ev->kind},
        {"ts",   ev->ts_ms},
        {"data", ev->data},
    };
}

} // namespace windbgmcp::handlers
