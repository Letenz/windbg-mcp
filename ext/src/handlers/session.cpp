// SPDX-License-Identifier: MIT
#include "handlers/handlers.h"

#include "events/publisher.h"
#include "ipc/pipe_server.h"
#include "util/debug_client.h"
#include "util/status.h"
#include "windbgmcp/protocol.h"

#include <DbgEng.h>
#include <atlbase.h>
#include <cstdio>

namespace windbgmcp::handlers {

using nlohmann::json;
using ipc::HandlerError;

namespace {

std::string HexU64(ULONG64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

const char* TargetKind() {
    auto client = dbg::Get();
    if (!client) return "none";
    CComQIPtr<IDebugControl4> ctl(client);
    if (!ctl) return "none";
    ULONG cls = 0, qual = 0;
    if (FAILED(ctl->GetDebuggeeType(&cls, &qual))) return "none";
    switch (cls) {
        case DEBUG_CLASS_KERNEL: return "kernel";
        case DEBUG_CLASS_USER_WINDOWS:
            if (qual == DEBUG_USER_WINDOWS_DUMP ||
                qual == DEBUG_USER_WINDOWS_DUMP_WINDOWS_CE) return "dump";
            return "user";
        case DEBUG_CLASS_UNINITIALIZED:
        default: return "none";
    }
}

} // namespace

json Session(std::int64_t /*req_id*/, const json& /*args*/, ipc::PipeServer& pipe,
             ipc::ConnectionGeneration generation) {
    auto client = dbg::Get();
    if (!client) {
        throw HandlerError(err::kEngineError, "DebugCreate failed", "restart WinDbg");
    }
    CComQIPtr<IDebugControl4> ctl(client);
    if (!ctl) {
        throw HandlerError(err::kEngineError, "QI IDebugControl4 failed", "");
    }

    // Serialise all KD-touching reads. Without this lock concurrent
    // handlers can race the KD transport and surface
    // "Kernel transport in use, packet ... failed" in WinDbg's log.
    std::lock_guard<std::mutex> engine_lk(dbg::Lock());

    ULONG exec = 0;
    ctl->GetExecutionStatus(&exec);

    const char* kind = TargetKind();
    const bool attached = std::string(kind) != "none";

    ULONG64 ip = 0;
    ULONG tid = 0;
    // Register/thread queries can issue KD traffic and may block indefinitely
    // while a live target is running. A liveness probe must stay cheap in
    // that state; detailed context is collected after wm_break_in.
    if (attached && !status::IsRunning(exec)) {
        if (CComQIPtr<IDebugRegisters2> regs(client); regs) {
            regs->GetInstructionOffset(&ip);
        }
        if (CComQIPtr<IDebugSystemObjects> sys(client); sys) {
            sys->GetCurrentThreadSystemId(&tid);
        }
    }

    // Bugcheck data, if any (only meaningful for kernel-mode dumps / live
    // KD that has crashed).  ReadBugCheckData issues a KD memory read, so
    // skip it when the target is still running; the cost otherwise is
    // unnecessary KD traffic plus possible "transport in use" failures.
    json bugcheck = nullptr;
    if (attached && !status::IsRunning(exec)) {
        ULONG bc_code = 0;
        ULONG64 bc_args[4] = {0};
        if (SUCCEEDED(ctl->ReadBugCheckData(&bc_code, &bc_args[0], &bc_args[1], &bc_args[2], &bc_args[3])) &&
            bc_code != 0) {
            char code_str[16];
            std::snprintf(code_str, sizeof(code_str), "0x%08lx", static_cast<unsigned long>(bc_code));
            json params = json::array();
            for (auto v : bc_args) params.push_back(HexU64(v));
            bugcheck = json{
                {"code",   code_str},
                {"params", params},
            };
        }
    }

    ULONG mod_loaded = 0, mod_unloaded = 0;
    if (attached && !status::IsRunning(exec)) {
        if (CComQIPtr<IDebugSymbols3> sym(client); sym) {
            sym->GetNumberModules(&mod_loaded, &mod_unloaded);
        }
    }

    return json{
        {"attached",      attached},
        {"target_kind",   kind},
        {"exec_status",   status::ToString(exec)},
        {"is_running",    status::IsRunning(exec)},
        {"is_broken",     status::IsBroken(exec)},
        {"ip",            HexU64(ip)},
        {"thread_id",     tid},
        {"bugcheck",      bugcheck},
        {"modules_count", mod_loaded},
        {"pipe_endpoint", pipe.Endpoint()},
        {"connection_generation", generation},
        {"event_queue_dropped",
         events::Publisher::Get().DroppedEvents()},
        {"engine_lane",   "serial"},
        {"ext_version",   "2.0.0"},
    };
}

void RegisterAll(ipc::Router& router); // forward; defined in handlers_register.cpp

} // namespace windbgmcp::handlers
