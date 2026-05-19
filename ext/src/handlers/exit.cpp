// SPDX-License-Identifier: MIT
//
// exit: detach the dbgeng session and signal the pipe layer to close.

#include "handlers/handlers.h"

#include "ipc/pipe_server.h"
#include "util/debug_client.h"
#include "util/log.h"
#include "windbgmcp/protocol.h"

#include <DbgEng.h>
#include <atlbase.h>

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

json Exit(std::int64_t /*req_id*/, const json& /*args*/, ipc::PipeServer& /*pipe*/) {
    auto client = dbg::Get();
    if (client) {
        // Best-effort detach. We do NOT call EndSession with terminate —
        // that would close WinDbg's UI. Detach is the friendliest option.
        if (CComQIPtr<IDebugClient> c2(client); c2) {
            c2->DetachProcesses();
        }
    }
    // The pipe will be closed by the exports.cpp `mcpext_stop` path or by
    // the next `!mcpext.stop`. We don't tear down the pipe from here, so
    // the resp can still go out cleanly before the user issues `.unload`.
    return json{{"ok", true}};
}

} // namespace windbgmcp::handlers
