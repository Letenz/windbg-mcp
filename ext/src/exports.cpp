// SPDX-License-Identifier: MIT
//
// WinDbg extension exports. The names mcpext_start / .stop / .status are
// what the user types as bang commands once the DLL is loaded:
//
//   .load windbgmcpExt
//   !mcpext.start
//   !mcpext.status
//   !mcpext.stop
//
// Plus the dbgeng extension entry points: DebugExtensionInitialize and
// friends. We only need Initialize/Uninitialize for our purposes.

#include "events/event_sink.h"
#include "events/publisher.h"
#include "handlers/handlers.h"
#include "ipc/pipe_server.h"
#include "ipc/router.h"
#include "util/debug_client.h"
#include "util/encoding.h"
#include "util/log.h"

#include <DbgEng.h>
#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace {
struct Globals {
    std::unique_ptr<windbgmcp::ipc::PipeServer> pipe;
    std::unique_ptr<windbgmcp::ipc::Router>     router;
    windbgmcp::events::EventSink*               sink = nullptr;
    std::atomic<bool>                           started{false};
};
Globals& G() { static Globals g; return g; }

void StopAllNoLock() {
    auto& g = G();
    if (g.sink) {
        g.sink->Uninstall();
        g.sink->Release();
        g.sink = nullptr;
    }
    if (g.router) g.router->Stop();
    if (g.pipe)   g.pipe->Stop();
    g.router.reset();
    g.pipe.reset();
    g.started.store(false);
}

// Helper: write a line to WinDbg's command window.
void Print(IDebugClient* client, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    CComQIPtr<IDebugControl> ctl(client);
    if (ctl) {
        ctl->Output(DEBUG_OUTPUT_NORMAL, "%s\n", buf);
    }
}
} // namespace

extern "C" {

// ---- dbgeng extension entry points ---------------------------------------

__declspec(dllexport) HRESULT CALLBACK
DebugExtensionInitialize(PULONG version, PULONG flags) {
    if (version) *version = MAKELONG(1, 0);
    if (flags)   *flags   = 0;
    return S_OK;
}

__declspec(dllexport) void CALLBACK
DebugExtensionUninitialize(void) {
    WMCP_LOG(Warn, "DebugExtensionUninitialize: dbgeng is unloading mcpext.dll");
    StopAllNoLock();
    windbgmcp::dbg::Reset();
}

__declspec(dllexport) HRESULT CALLBACK
DebugExtensionNotify(ULONG notify, ULONG64 argument) {
    const char* name = "?";
    switch (notify) {
        case DEBUG_NOTIFY_SESSION_ACTIVE:        name = "SESSION_ACTIVE";       break;
        case DEBUG_NOTIFY_SESSION_INACTIVE:      name = "SESSION_INACTIVE";     break;
        case DEBUG_NOTIFY_SESSION_ACCESSIBLE:    name = "SESSION_ACCESSIBLE";   break;
        case DEBUG_NOTIFY_SESSION_INACCESSIBLE:  name = "SESSION_INACCESSIBLE"; break;
    }
    WMCP_LOG(Info, std::string("DebugExtensionNotify: ") + name +
                   " (raw=" + std::to_string(notify) + ", arg=" +
                   std::to_string(argument) + ")");
    return S_OK;
}

// ---- !mcpext bang commands -----------------------------------------------

__declspec(dllexport) HRESULT CALLBACK
start(PDEBUG_CLIENT client, PCSTR /*args*/) {
    auto& g = G();
    if (g.started.load()) {
        Print(client, "windbgmcp: already running on \\\\.\\pipe\\windbgmcp");
        return S_OK;
    }

    g.pipe   = std::make_unique<windbgmcp::ipc::PipeServer>();
    g.router = std::make_unique<windbgmcp::ipc::Router>();
    g.router->SetPipe(g.pipe.get());

    windbgmcp::handlers::RegisterAll(*g.router);
    g.router->Start(/*worker_count=*/4);

    bool ok = g.pipe->Start([](std::string payload) {
        G().router->OnPayload(std::move(payload));
    });
    if (!ok) {
        Print(client, "windbgmcp: FAILED to open pipe \\\\.\\pipe\\windbgmcp "
                      "(another instance running?)");
        StopAllNoLock();
        return E_FAIL;
    }

    // Wire the publisher to the pipe so events flow.
    windbgmcp::events::Publisher::Get().SetPipe(g.pipe.get());

    // Install the event sink on the process-wide IDebugClient.
    g.sink = windbgmcp::events::EventSink::Create();
    if (!g.sink->Install()) {
        Print(client, "windbgmcp: WARNING failed to install event sink "
                      "(events will not be pushed)");
    }

    g.started.store(true);
    Print(client, "windbgmcp: listening on \\\\.\\pipe\\windbgmcp");
    // Force-create the log file so the operator can verify the path early,
    // before any host connects.
    WMCP_LOG(Info, "mcpext.start: pipe listening, event sink installed");
    return S_OK;
}

__declspec(dllexport) HRESULT CALLBACK
stop(PDEBUG_CLIENT client, PCSTR /*args*/) {
    WMCP_LOG(Warn, "!mcpext.stop invoked by user");
    if (!G().started.load()) {
        Print(client, "windbgmcp: not running");
        return S_OK;
    }
    StopAllNoLock();
    Print(client, "windbgmcp: stopped");
    return S_OK;
}

__declspec(dllexport) HRESULT CALLBACK
status(PDEBUG_CLIENT client, PCSTR /*args*/) {
    auto& g = G();
    if (!g.started.load()) {
        Print(client, "windbgmcp: not running. Use !mcpext.start to begin.");
        return S_OK;
    }
    Print(client,
          "windbgmcp: running\n"
          "  pipe       : \\\\.\\pipe\\windbgmcp\n"
          "  connected  : %s\n"
          "  event_sink : %s",
          g.pipe && g.pipe->IsConnected() ? "yes" : "no",
          g.sink ? "installed" : "MISSING");
    return S_OK;
}

__declspec(dllexport) HRESULT CALLBACK
help(PDEBUG_CLIENT client, PCSTR /*args*/) {
    Print(client,
          "mcpext - WinDbg <-> MCP bridge\n"
          "  !mcpext.start    open the named pipe and start serving the MCP host\n"
          "  !mcpext.stop     close the pipe and stop the event sink\n"
          "  !mcpext.status   show pipe and connection status\n"
          "  !mcpext.help     this message");
    return S_OK;
}

} // extern "C"
