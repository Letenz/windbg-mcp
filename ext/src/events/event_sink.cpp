// SPDX-License-Identifier: MIT
#include "events/event_sink.h"

#include "events/publisher.h"
#include "util/debug_client.h"
#include "util/log.h"
#include "util/status.h"

#include <cstdio>
#include <string>

// STATUS_BREAKPOINT is the only NTSTATUS we need; define it locally to avoid
// the ntstatus.h / winnt.h dual-definition warning cascade.
#ifndef STATUS_BREAKPOINT
#define STATUS_BREAKPOINT ((ULONG)0x80000003L)
#endif

namespace windbgmcp::events {

using nlohmann::json;

namespace {

// Static table of common bugcheck names. Driver-specific codes (or anything
// not in this table) come back as "UNKNOWN" — the AI gets the hex code
// either way, and the analyzer tool can resolve names with !analyze.
const char* BugcheckName(ULONG code) {
    switch (code) {
        case 0x0000001E: return "KMODE_EXCEPTION_NOT_HANDLED";
        case 0x00000050: return "PAGE_FAULT_IN_NONPAGED_AREA";
        case 0x000000C2: return "BAD_POOL_CALLER";
        case 0x000000C4: return "DRIVER_VERIFIER_DETECTED_VIOLATION";
        case 0x000000C5: return "DRIVER_CORRUPTED_EXPOOL";
        case 0x000000CA: return "PNP_DETECTED_FATAL_ERROR";
        case 0x000000D1: return "DRIVER_IRQL_NOT_LESS_OR_EQUAL";
        case 0x000000D5: return "DRIVER_PAGE_FAULT_IN_FREED_SPECIAL_POOL";
        case 0x000000D6: return "DRIVER_PAGE_FAULT_BEYOND_END_OF_ALLOCATION";
        case 0x000000DA: return "SYSTEM_PTE_MISUSE";
        case 0x000000E1: return "WORKER_THREAD_RETURNED_AT_BAD_IRQL";
        case 0x000000F4: return "CRITICAL_OBJECT_TERMINATION";
        case 0x000000FC: return "ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY";
        case 0x0000003B: return "SYSTEM_SERVICE_EXCEPTION";
        case 0x0000007E: return "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED";
        case 0x0000007F: return "UNEXPECTED_KERNEL_MODE_TRAP";
        case 0x00000139: return "KERNEL_SECURITY_CHECK_FAILURE";
        case 0x00000124: return "WHEA_UNCORRECTABLE_ERROR";
        case 0x000000EF: return "CRITICAL_PROCESS_DIED";
        default:         return "UNKNOWN";
    }
}

std::string HexU64(ULONG64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}
std::string HexU32(ULONG v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(v));
    return buf;
}

// Try to resolve a faulting module name + offset for the given IP.
json ResolveFaultingModule(ULONG64 ip) {
    auto client = dbg::Get();
    if (!client) return nullptr;
    CComQIPtr<IDebugSymbols3> sym(client);
    if (!sym) return nullptr;

    ULONG64 mod_base = 0;
    ULONG mod_index  = 0;
    if (FAILED(sym->GetModuleByOffset(ip, 0, &mod_index, &mod_base))) {
        return nullptr;
    }
    char name[256] = {0};
    ULONG name_size = 0;
    if (FAILED(sym->GetModuleNames(mod_index, mod_base,
                                   nullptr, 0, nullptr,
                                   name, sizeof(name), &name_size,
                                   nullptr, 0, nullptr))) {
        return nullptr;
    }
    return json{
        {"name",   std::string(name)},
        {"base",   HexU64(mod_base)},
        {"offset", HexU64(ip - mod_base)},
    };
}

ULONG CurrentThreadId() {
    auto client = dbg::Get();
    if (!client) return 0;
    CComQIPtr<IDebugSystemObjects> sys(client);
    if (!sys) return 0;
    ULONG tid = 0;
    sys->GetCurrentThreadSystemId(&tid);
    return tid;
}

ULONG64 CurrentIp() {
    auto client = dbg::Get();
    if (!client) return 0;
    CComQIPtr<IDebugRegisters2> regs(client);
    if (!regs) return 0;
    ULONG64 ip = 0;
    if (FAILED(regs->GetInstructionOffset(&ip))) return 0;
    return ip;
}

} // namespace

EventSink* EventSink::Create() {
    return new EventSink();
}

// ---- IUnknown ------------------------------------------------------------
STDMETHODIMP EventSink::QueryInterface(REFIID iid, PVOID* iface) {
    if (!iface) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDebugEventCallbacks)) {
        *iface = static_cast<IDebugEventCallbacks*>(this);
        AddRef();
        return S_OK;
    }
    *iface = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) EventSink::AddRef()  { return ++m_ref; }
STDMETHODIMP_(ULONG) EventSink::Release() {
    ULONG r = --m_ref;
    if (r == 0) delete this;
    return r;
}

// ---- IDebugEventCallbacks ------------------------------------------------
STDMETHODIMP EventSink::GetInterestMask(PULONG mask) {
    if (!mask) return E_POINTER;
    *mask =
        DEBUG_EVENT_BREAKPOINT          |
        DEBUG_EVENT_EXCEPTION           |
        DEBUG_EVENT_LOAD_MODULE         |
        DEBUG_EVENT_UNLOAD_MODULE       |
        DEBUG_EVENT_SESSION_STATUS      |
        DEBUG_EVENT_CHANGE_ENGINE_STATE;
    return S_OK;
}

STDMETHODIMP EventSink::Breakpoint(PDEBUG_BREAKPOINT bp) {
    if (bp) {
        ULONG id = 0;
        bp->GetId(&id);
        m_last_bp_id.store(id);

        ULONG64 addr = 0; bp->GetOffset(&addr);
        ULONG hit_count = 0; bp->GetCurrentPassCount(&hit_count);

        char expr[256] = {0};
        ULONG expr_used = 0;
        bp->GetOffsetExpression(expr, sizeof(expr), &expr_used);

        Event ev{
            NowMs(), "breakpoint_hit",
            json{
                {"bp_id",      id},
                {"address",    HexU64(addr)},
                {"expression", expr_used > 0 ? std::string(expr) : std::string()},
                {"hit_count",  hit_count},
            }
        };
        Publisher::Get().Publish(std::move(ev));
    }
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventSink::Exception(PEXCEPTION_RECORD64 ex, ULONG /*first_chance*/) {
    if (!ex) return DEBUG_STATUS_NO_CHANGE;
    // Bugcheck pattern: STATUS_BREAKPOINT (0x80000003) with first param == bugcheck code.
    if (ex->ExceptionCode == STATUS_BREAKPOINT && ex->NumberParameters >= 1) {
        const ULONG bc = static_cast<ULONG>(ex->ExceptionInformation[0]);
        // Only fire for non-zero bugcheck codes; 0 means a genuine int 3.
        if (bc != 0) {
            json params = json::array();
            for (ULONG i = 1; i <= 4 && i < ex->NumberParameters; ++i) {
                params.push_back(HexU64(ex->ExceptionInformation[i]));
            }
            const ULONG64 ip = ex->ExceptionAddress;
            Event ev{
                NowMs(), "bugcheck",
                json{
                    {"code",            HexU32(bc)},
                    {"name",            BugcheckName(bc)},
                    {"params",          params},
                    {"ip",              HexU64(ip)},
                    {"faulting_module", ResolveFaultingModule(ip)},
                    {"thread_id",       CurrentThreadId()},
                }
            };
            m_pending_bugcheck.store(true);
            Publisher::Get().Publish(std::move(ev));
        }
    }
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventSink::CreateThread(ULONG64, ULONG64, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }
STDMETHODIMP EventSink::ExitThread(ULONG)                        { return DEBUG_STATUS_NO_CHANGE; }
STDMETHODIMP EventSink::CreateProcess(ULONG64, ULONG64, ULONG64, ULONG, PCSTR, PCSTR, ULONG, ULONG,
                                      ULONG64, ULONG64, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }
STDMETHODIMP EventSink::ExitProcess(ULONG)                       { return DEBUG_STATUS_NO_CHANGE; }

STDMETHODIMP EventSink::LoadModule(ULONG64 /*image_handle*/, ULONG64 base, ULONG size,
                                   PCSTR mod_name, PCSTR image_name, ULONG /*chk*/, ULONG ts) {
    Event ev{
        NowMs(), "module_load",
        json{
            {"name",      mod_name   ? mod_name   : ""},
            {"image",     image_name ? image_name : ""},
            {"base",      HexU64(base)},
            {"size",      size},
            {"timestamp", ts},
        }
    };
    Publisher::Get().Publish(std::move(ev));
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventSink::UnloadModule(PCSTR image_base_name, ULONG64 base) {
    Event ev{
        NowMs(), "module_unload",
        json{
            {"name", image_base_name ? image_base_name : ""},
            {"base", HexU64(base)},
        }
    };
    Publisher::Get().Publish(std::move(ev));
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventSink::SystemError(ULONG, ULONG)         { return DEBUG_STATUS_NO_CHANGE; }

STDMETHODIMP EventSink::SessionStatus(ULONG status) {
    const char* s = "active";
    switch (status) {
        case DEBUG_SESSION_ACTIVE:                  s = "active";    break;
        case DEBUG_SESSION_END_SESSION_ACTIVE_TERMINATE:
        case DEBUG_SESSION_END_SESSION_ACTIVE_DETACH:
        case DEBUG_SESSION_END_SESSION_PASSIVE:
        case DEBUG_SESSION_END:                     s = "end";       break;
        case DEBUG_SESSION_REBOOT:                  s = "reboot";    break;
        case DEBUG_SESSION_HIBERNATE:               s = "hibernate"; break;
        case DEBUG_SESSION_FAILURE:                 s = "failure";   break;
        default: break;
    }
    Event ev{
        NowMs(), "session_status",
        json{{"status", s}, {"detail", json(nullptr)}}
    };
    Publisher::Get().Publish(std::move(ev));
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventSink::ChangeDebuggeeState(ULONG, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }

STDMETHODIMP EventSink::ChangeEngineState(ULONG flags, ULONG64 argument) {
    if (!(flags & DEBUG_CES_EXECUTION_STATUS)) {
        return DEBUG_STATUS_NO_CHANGE;
    }

    const ULONG new_status = static_cast<ULONG>(argument & 0xFFFFFFFFu);
    const ULONG prev = m_last_status.exchange(new_status);

    // Always publish the raw state_change event.
    {
        Event ev{
            NowMs(), "state_change",
            json{
                {"from", status::ToString(prev)},
                {"to",   status::ToString(new_status)},
            }
        };
        Publisher::Get().Publish(std::move(ev));
    }

    // Coalesce: if a bugcheck is pending, skip the regular break event.
    const bool went_to_break = !status::IsRunning(new_status) && status::IsBroken(new_status);
    const bool went_to_break_like = !status::IsRunning(new_status); // any non-running
    if (went_to_break || went_to_break_like) {
        if (m_pending_bugcheck.exchange(false)) {
            return DEBUG_STATUS_NO_CHANGE; // suppressed, bugcheck already published
        }
        const char* reason = "other";
        if (m_last_bp_id.exchange(0) != 0)            reason = "breakpoint";
        else if (status::IsRunning(prev))             reason = "interrupt";

        Event ev{
            NowMs(), "break",
            json{
                {"reason",         reason},
                {"ip",             HexU64(CurrentIp())},
                {"thread_id",      CurrentThreadId()},
                {"prior_status",   status::ToString(prev)},
                {"current_status", status::ToString(new_status)},
            }
        };
        Publisher::Get().Publish(std::move(ev));
    }

    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP EventSink::ChangeSymbolState(ULONG, ULONG64) { return DEBUG_STATUS_NO_CHANGE; }

// ---- Install / Uninstall -------------------------------------------------
bool EventSink::Install() {
    auto client = dbg::Get();
    if (!client) return false;
    return SUCCEEDED(client->SetEventCallbacks(this));
}

void EventSink::Uninstall() {
    auto client = dbg::Get();
    if (!client) return;
    client->SetEventCallbacks(nullptr);
}

} // namespace windbgmcp::events
