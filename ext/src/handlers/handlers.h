// SPDX-License-Identifier: MIT
//
// Per-op handlers, registered with the router at extension startup.
// Declared together for brevity; one .cpp per op so blame is clean.

#pragma once

#include "ipc/router.h"

namespace windbgmcp::handlers {

nlohmann::json Session(std::int64_t req_id, const nlohmann::json& args, ipc::PipeServer& pipe);
nlohmann::json RunCmd (std::int64_t req_id, const nlohmann::json& args, ipc::PipeServer& pipe);
nlohmann::json BreakIn(std::int64_t req_id, const nlohmann::json& args, ipc::PipeServer& pipe);
nlohmann::json Exit   (std::int64_t req_id, const nlohmann::json& args, ipc::PipeServer& pipe);

// wait_event lives in router-land too because it just blocks on the
// publisher; we declare it here for symmetry.
nlohmann::json WaitEvent(std::int64_t req_id, const nlohmann::json& args, ipc::PipeServer& pipe);

void RegisterAll(ipc::Router& router);

} // namespace windbgmcp::handlers
