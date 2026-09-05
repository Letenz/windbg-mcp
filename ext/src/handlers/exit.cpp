// SPDX-License-Identifier: MIT
//
// Explicit lifecycle operations:
//   detach   - detach the target only; leave the bridge running
//   shutdown - return metadata; Router sends a final response and schedules
//              bridge teardown without touching the target
//   exit     - deprecated compatibility alias for detach

#include "handlers/handlers.h"

#include "ipc/pipe_server.h"
#include "util/debug_client.h"
#include "util/log.h"
#include "windbgmcp/protocol.h"

#include <DbgEng.h>
#include <atlbase.h>

#include <mutex>

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

namespace {

json DetachTarget(ipc::PipeServer& pipe,
                  ipc::ConnectionGeneration generation,
                  bool deprecated) {
    auto client = dbg::Get();
    if (!client) {
        throw HandlerError(err::kEngineError, "no IDebugClient", "");
    }
    CComQIPtr<IDebugClient> detach_client(client);
    if (!detach_client) {
        throw HandlerError(err::kEngineError, "QI IDebugClient failed", "");
    }
    CComQIPtr<IDebugControl> control(client);
    if (!control) {
        throw HandlerError(err::kEngineError, "QI IDebugControl failed", "");
    }

    HRESULT hr = S_OK;
    const char* detach_api = nullptr;
    const char* target_kind = nullptr;
    bool target_detached = true;
    {
        std::lock_guard<std::mutex> engine_lk(dbg::Lock());
        ULONG target_class = DEBUG_CLASS_UNINITIALIZED;
        ULONG target_qualifier = 0;
        hr = control->GetDebuggeeType(&target_class, &target_qualifier);
        if (FAILED(hr)) {
            throw HandlerError(err::kEngineError,
                               "IDebugControl::GetDebuggeeType failed", "", hr);
        }
        if (target_class == DEBUG_CLASS_UNINITIALIZED) {
            throw HandlerError(err::kNotAttached, "no debug target attached", "");
        }
        // DetachProcesses is a user-process operation.  A live kernel
        // connection belongs to the debugger session and must be disconnected
        // with EndSession(DEBUG_END_ACTIVE_DETACH); this does not terminate or
        // modify the kernel target.  Dump/image sessions have no live target,
        // so passive cleanup is the only truthful interpretation of detach.
        if (target_class == DEBUG_CLASS_KERNEL) {
            target_kind = "kernel";
            detach_api = "IDebugClient::EndSession(DEBUG_END_ACTIVE_DETACH)";
            hr = detach_client->EndSession(DEBUG_END_ACTIVE_DETACH);
        } else if (target_class == DEBUG_CLASS_USER_WINDOWS) {
            target_kind = "user_windows";
            detach_api = "IDebugClient::DetachProcesses";
            hr = detach_client->DetachProcesses();
        } else if (target_class == DEBUG_CLASS_IMAGE_FILE) {
            target_kind = "image_file";
            detach_api = "IDebugClient::EndSession(DEBUG_END_PASSIVE)";
            target_detached = false;
            hr = detach_client->EndSession(DEBUG_END_PASSIVE);
        } else {
            throw HandlerError(err::kInvalidArg,
                               "unsupported debuggee class for detach", "");
        }
    }
    if (FAILED(hr)) {
        throw HandlerError(err::kEngineError,
                           std::string(detach_api ? detach_api : "detach") + " failed",
                           "the target may already be detached; inspect wm_session",
                           hr);
    }

    json result = {
        {"ok", true},
        {"semantic", "detach_target_only"},
        {"target_detached", target_detached},
        {"target_kind", target_kind},
        {"detach_api", detach_api},
        {"bridge_state", pipe.IsRunning() ? "running" : "stopping"},
        {"connection_generation", generation},
    };
    if (deprecated) {
        result["deprecated"] = true;
        result["replacement"] = "wm_detach";
    }
    return result;
}

} // namespace

json Detach(std::int64_t /*req_id*/, const json& /*args*/, ipc::PipeServer& pipe,
            ipc::ConnectionGeneration generation) {
    return DetachTarget(pipe, generation, false);
}

json Shutdown(std::int64_t /*req_id*/, const json& /*args*/, ipc::PipeServer& /*pipe*/,
              ipc::ConnectionGeneration generation) {
    // Router owns the ordering guarantee: it writes this response through
    // PipeServer::SendFinal and only then invokes the async teardown action.
    // No dbgeng API is called, so the target remains attached and unchanged.
    return json{
        {"ok", true},
        {"semantic", "bridge_shutdown_only"},
        {"target_detached", false},
        {"bridge_state", "stopping_after_response"},
        {"host_state", "running"},
        {"connection_generation", generation},
    };
}

json Exit(std::int64_t /*req_id*/, const json& /*args*/, ipc::PipeServer& pipe,
          ipc::ConnectionGeneration generation) {
    return DetachTarget(pipe, generation, true);
}

} // namespace windbgmcp::handlers
