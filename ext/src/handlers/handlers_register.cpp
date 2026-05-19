// SPDX-License-Identifier: MIT
#include "handlers/handlers.h"

namespace windbgmcp::handlers {

void RegisterAll(ipc::Router& router) {
    router.Register("session",    &Session);
    router.Register("run_cmd",    &RunCmd);
    router.Register("wait_event", &WaitEvent);
    router.Register("break_in",   &BreakIn);
    router.Register("exit",       &Exit);
}

} // namespace windbgmcp::handlers
