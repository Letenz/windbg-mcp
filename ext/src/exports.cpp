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
#include "lifecycle/session_state.h"
#include "lifecycle/unload_barrier.h"
#include "util/encoding.h"
#include "util/log.h"
#include "windbgmcp/pipe_endpoint.h"

#include <DbgEng.h>
#include <Windows.h>

#include <array>
#include <condition_variable>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

namespace {
struct SessionResources {
    std::shared_ptr<windbgmcp::ipc::PipeServer> pipe;
    std::shared_ptr<windbgmcp::ipc::Router> router;
    windbgmcp::events::EventSink* sink = nullptr;
};

enum class ReaperCommand {
    Wait,
    Teardown,
    Cancel,
};

struct ReaperControl {
    std::mutex mu;
    std::condition_variable cv;
    ReaperCommand command = ReaperCommand::Wait;
    SessionResources resources;
};

struct Globals {
    std::mutex                                  lifecycle_mu;
    windbgmcp::lifecycle::SessionStateMachine   lifecycle;
    std::shared_ptr<windbgmcp::ipc::PipeServer> pipe;
    std::shared_ptr<windbgmcp::ipc::Router>     router;
    windbgmcp::events::EventSink*               sink = nullptr;
    std::shared_ptr<ReaperControl>               teardown_control;
    std::thread                                 teardown_thread;
    bool                                        teardown_complete = false;
};

// Intentionally process-lifetime storage. Normal !mcpext.stop and
// wm_shutdown move all owned resources to an asynchronous reaper. No caller
// ever destroys or joins the Router while executing on its worker thread.
Globals& G() { static Globals* g = new Globals(); return *g; }

enum class StopResult {
    Ready,
    Stopped,
    AlreadyStopped,
    Busy,
    Scheduled,
};

struct StopOperation {
    StopResult result = StopResult::Busy;
    SessionResources resources;
    std::shared_ptr<ReaperControl> control;
};

std::string NewBridgeInstanceId() {
    std::random_device random;
    std::array<std::uint32_t, 4> words{};
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    for (auto& word : words) word = random();
    words[0] ^= ::GetCurrentProcessId();
    words[1] ^= ::GetCurrentThreadId();
    words[2] ^= static_cast<std::uint32_t>(counter.QuadPart);
    words[3] ^= static_cast<std::uint32_t>(counter.QuadPart >> 32);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto word : words) out << std::setw(8) << word;
    return out.str();
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

StopOperation BeginStop(Globals& g) {
    using windbgmcp::lifecycle::SessionState;

    std::lock_guard<std::mutex> lk(g.lifecycle_mu);

    const auto state = g.lifecycle.State();
    if (state == SessionState::Stopped) {
        return {StopResult::AlreadyStopped, {}, {}};
    }
    if (state == SessionState::Starting || state == SessionState::Stopping) {
        return {StopResult::Busy, {}, {}};
    }
    if (!g.teardown_control || !g.teardown_thread.joinable()) {
        WMCP_LOG(Error,
                 "stop refused: prepared teardown executor is unavailable");
        return {StopResult::Busy, {}, {}};
    }
    {
        std::lock_guard<std::mutex> control_lk(g.teardown_control->mu);
        if (g.teardown_control->command != ReaperCommand::Wait) {
            WMCP_LOG(Error,
                     "stop refused: prepared teardown executor is not idle");
            return {StopResult::Busy, {}, {}};
        }
    }

    if (!g.lifecycle.BeginStop()) {
        return {StopResult::Busy, {}, {}};
    }

    SessionResources resources{
        std::move(g.pipe), std::move(g.router), g.sink};
    g.sink = nullptr;
    return {StopResult::Ready, std::move(resources), g.teardown_control};
}

bool Teardown(SessionResources& resources) {
    windbgmcp::events::Publisher::Get().ClearPipe();
    if (resources.pipe) resources.pipe->Stop();
    windbgmcp::events::Publisher::Get().CancelWaiters();

    // Stop the callback pump before joining the request lane. Otherwise the
    // pump can repeatedly reacquire dbgeng while Router::Stop waits for its
    // worker to release the request client.
    if (resources.sink && !resources.sink->Uninstall()) {
        WMCP_LOG(Error, "event sink uninstall failed on owner thread");
        return false;
    }

    if (resources.router && !resources.router->Stop()) {
        // Do not reset/delete it while its worker is alive. The caller keeps
        // ownership in Stopping state and the unload barrier remains closed.
        return false;
    }

    if (resources.sink) {
        resources.sink->Release();
        resources.sink = nullptr;
    }
    resources.router.reset();
    resources.pipe.reset();
    return true;
}

bool PrepareReaper(
    Globals& g,
    std::shared_ptr<ReaperControl>& control,
    std::thread& thread) {
    try {
        control = std::make_shared<ReaperControl>();
        thread = std::thread([&g, control] {
            {
                std::unique_lock<std::mutex> lk(control->mu);
                control->cv.wait(lk, [&] {
                    return control->command != ReaperCommand::Wait;
                });
                if (control->command == ReaperCommand::Cancel) return;
            }

            windbgmcp::lifecycle::ActivityGuard teardown_activity(
                windbgmcp::lifecycle::ActivityKind::Teardown);
            bool stopped = false;
            try {
                stopped = Teardown(control->resources);
            } catch (const std::exception& e) {
                (void)e;
                WMCP_LOG(Error,
                         std::string("teardown threw; lifecycle remains stopping: ") +
                             e.what());
            } catch (...) {
                WMCP_LOG(Error,
                         "teardown threw; lifecycle remains stopping");
            }

            std::lock_guard<std::mutex> lifecycle_lk(g.lifecycle_mu);
            if (stopped) {
                g.lifecycle.FinishStop();
            } else {
                // Teardown has externally visible, irreversible side effects
                // (the terminal pipe may already be fenced). Never roll back
                // to a false Running state.
                WMCP_LOG(Error,
                         "teardown incomplete; lifecycle held in stopping");
            }
            g.teardown_complete = true;
        });
        return true;
    } catch (...) {
        control.reset();
        return false;
    }
}

void CancelPreparedReaper(
    const std::shared_ptr<ReaperControl>& control,
    std::thread& thread) noexcept {
    if (control) {
        {
            std::lock_guard<std::mutex> lk(control->mu);
            control->command = ReaperCommand::Cancel;
        }
        control->cv.notify_one();
    }
    if (thread.joinable()) thread.join();
}

StopResult StopSession(Globals& g) {
    auto operation = BeginStop(g);
    if (operation.result != StopResult::Ready) return operation.result;

    // The executor thread and its resource storage were created before
    // lifecycle=Running was committed. Arming it is allocation-free and
    // cannot fail after a terminal response has been delivered.
    {
        std::lock_guard<std::mutex> lk(g.lifecycle_mu);
        g.teardown_complete = false;
    }
    {
        std::lock_guard<std::mutex> control_lk(operation.control->mu);
        if (operation.control->command != ReaperCommand::Wait) {
            WMCP_LOG(Error, "prepared teardown executor was already armed");
            // Do not claim Running again: resources have already moved and a
            // terminal response may have committed. Stopping is fail-closed.
            return StopResult::Busy;
        }
        operation.control->resources = std::move(operation.resources);
        operation.control->command = ReaperCommand::Teardown;
    }
    operation.control->cv.notify_one();
    return StopResult::Scheduled;
}

void JoinCompletedTeardown(Globals& g) {
    std::thread completed;
    bool had_completed = false;
    {
        std::lock_guard<std::mutex> lk(g.lifecycle_mu);
        if (g.teardown_complete && g.teardown_thread.joinable()) {
            completed = std::move(g.teardown_thread);
            g.teardown_complete = false;
            had_completed = true;
        }
    }
    if (completed.joinable()) completed.join();
    if (had_completed) {
        std::lock_guard<std::mutex> lk(g.lifecycle_mu);
        if (g.lifecycle.State() ==
            windbgmcp::lifecycle::SessionState::Stopped) {
            g.teardown_control.reset();
        }
    }
}

void FinishStartFailure(Globals& g) {
    std::lock_guard<std::mutex> lk(g.lifecycle_mu);
    g.lifecycle.FinishStart(false);
}

windbgmcp::lifecycle::SessionState CurrentState(Globals& g) {
    std::lock_guard<std::mutex> lk(g.lifecycle_mu);
    return g.lifecycle.State();
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
    auto& g = G();
    const auto result = StopSession(g);
    if (result == StopResult::Busy) {
        WMCP_LOG(Error,
                 "DebugExtensionUninitialize could not schedule teardown; "
                 "lifecycle is already transitioning");
    }
}

__declspec(dllexport) HRESULT CALLBACK
DebugExtensionCanUnload(void) {
    auto& g = G();
    JoinCompletedTeardown(g);
    std::lock_guard<std::mutex> lk(g.lifecycle_mu);
    if (g.lifecycle.State() != windbgmcp::lifecycle::SessionState::Stopped) {
        return S_FALSE;
    }
    return windbgmcp::lifecycle::UnloadBarrier::Get().Snapshot().Quiescent()
               ? S_OK
               : S_FALSE;
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
start(PDEBUG_CLIENT client, PCSTR args) {
    auto& g = G();
    JoinCompletedTeardown(g);
    const auto parsed = windbgmcp::NormalizePipeEndpoint(args ? args : "");
    if (!parsed.ok) {
        Print(client, "windbgmcp: invalid pipe endpoint: %s", parsed.error.c_str());
        Print(client, "usage: !mcpext.start [pipe-name|\\\\.\\pipe\\pipe-name]");
        return E_INVALIDARG;
    }

    using windbgmcp::lifecycle::SessionState;
    {
        std::lock_guard<std::mutex> lk(g.lifecycle_mu);
        if (g.lifecycle.State() != SessionState::Stopped) {
            Print(client, "windbgmcp: cannot start while lifecycle state is %s%s%s",
                  windbgmcp::lifecycle::ToString(g.lifecycle.State()),
                  g.pipe ? " on " : "",
                  g.pipe ? g.pipe->Endpoint().c_str() : "");
            return g.lifecycle.State() == SessionState::Running
                       ? S_OK
                       : HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        if (!g.lifecycle.BeginStart()) return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    windbgmcp::events::Publisher::Get().ResumeWaiters();

    std::shared_ptr<ReaperControl> prepared_control;
    std::thread prepared_reaper;
    if (!PrepareReaper(g, prepared_control, prepared_reaper)) {
        FinishStartFailure(g);
        Print(client,
              "windbgmcp: start failed while preparing teardown executor");
        return E_OUTOFMEMORY;
    }

    std::shared_ptr<windbgmcp::ipc::PipeServer> pipe;
    std::shared_ptr<windbgmcp::ipc::Router> router;
    try {
        const std::string bridge_instance_id = NewBridgeInstanceId();
        pipe = std::make_shared<windbgmcp::ipc::PipeServer>(parsed.endpoint);
        router = std::make_shared<windbgmcp::ipc::Router>();
        router->SetPipe(pipe.get());

        windbgmcp::handlers::RegisterAll(*router);
        router->RegisterFinalResponseAction(
            "shutdown",
            [](windbgmcp::ipc::ConnectionGeneration generation) {
                // This callback runs on the Router worker after SendFinal
                // has written the response and fenced generation. StopSession
                // only moves ownership and starts a reaper; it never joins
                // the current worker or enters dbgeng synchronously.
                const auto result = StopSession(G());
                if (result != StopResult::Scheduled &&
                    result != StopResult::AlreadyStopped) {
                    WMCP_LOG(Error,
                             "wm_shutdown response was delivered for generation=" +
                                 std::to_string(generation) +
                                 " but async teardown could not be scheduled");
                }
            });
        // All dbgeng request handlers share one engine client. Keep them on
        // one stable worker thread to obey client thread affinity.
        router->Start(/*worker_count=*/1);

        std::weak_ptr<windbgmcp::ipc::Router> weak_router = router;
        const std::string greeting = nlohmann::json{
            {"frame", "hello"},
            {"protocol_version", windbgmcp::kProtocolVersion},
            {"bridge_instance_id", bridge_instance_id},
            {"pipe_endpoint", parsed.endpoint},
        }.dump();
        const bool ok = pipe->Start(
            [weak_router](
                std::string payload,
                windbgmcp::ipc::ConnectionGeneration generation) {
                if (auto target = weak_router.lock()) {
                    target->OnPayload(std::move(payload), generation);
                }
            },
            greeting);
        if (!ok) {
            Print(client, "windbgmcp: FAILED to open pipe %s "
                          "(endpoint already in use or unavailable; gle=%lu)",
                  parsed.endpoint.c_str(),
                  static_cast<unsigned long>(pipe->LastError()));
            router->Stop();
            pipe->Stop();
            CancelPreparedReaper(prepared_control, prepared_reaper);
            FinishStartFailure(g);
            return E_FAIL;
        }
    } catch (const std::exception& e) {
        if (pipe) pipe->Stop();
        if (router) router->Stop();
        CancelPreparedReaper(prepared_control, prepared_reaper);
        FinishStartFailure(g);
        Print(client, "windbgmcp: start failed: %s", e.what());
        return E_FAIL;
    } catch (...) {
        if (pipe) pipe->Stop();
        if (router) router->Stop();
        CancelPreparedReaper(prepared_control, prepared_reaper);
        FinishStartFailure(g);
        Print(client, "windbgmcp: start failed with an unknown exception");
        return E_FAIL;
    }

    // Event client initialization is deliberately asynchronous. This bang
    // command runs while WinDbg owns an internal engine lock; synchronously
    // waiting for another thread's DebugCreate would deadlock startup.
    windbgmcp::events::EventSink* sink = nullptr;
    try {
        sink = windbgmcp::events::EventSink::Create();
        const bool sink_started = sink->InstallAsync();
        if (!sink_started) {
            sink->Release();
            sink = nullptr;
            Print(client, "windbgmcp: WARNING failed to start dedicated event client "
                          "(events will not be pushed)");
        }
    } catch (...) {
        if (sink) {
            sink->Uninstall();
            sink->Release();
            sink = nullptr;
        }
        pipe->Stop();
        router->Stop();
        CancelPreparedReaper(prepared_control, prepared_reaper);
        FinishStartFailure(g);
        Print(client, "windbgmcp: start failed while creating the event client");
        return E_FAIL;
    }

    // Wire the publisher to the pipe so events flow.
    windbgmcp::events::Publisher::Get().SetPipe(pipe);
    {
        std::lock_guard<std::mutex> lk(g.lifecycle_mu);
        g.pipe = pipe;
        g.router = router;
        g.sink = sink;
        g.teardown_control = std::move(prepared_control);
        g.teardown_thread = std::move(prepared_reaper);
        g.teardown_complete = false;
        g.lifecycle.FinishStart(true);
    }
    // This is the only request-gate opening point. All resources, including
    // the allocation/thread needed by shutdown, are now durably published.
    router->CommitStart();
    Print(client, "windbgmcp: listening on %s", parsed.endpoint.c_str());
    // Force-create the log file so the operator can verify the path early,
    // before any host connects.
    WMCP_LOG(Info, "mcpext.start: pipe listening, event sink initialization queued");
    return S_OK;
}

__declspec(dllexport) HRESULT CALLBACK
stop(PDEBUG_CLIENT client, PCSTR /*args*/) {
    WMCP_LOG(Warn, "!mcpext.stop invoked by user");
    auto& g = G();
    JoinCompletedTeardown(g);
    const auto result = StopSession(g);
    switch (result) {
        case StopResult::Ready:
            return E_UNEXPECTED;
        case StopResult::Stopped:
            Print(client, "windbgmcp: stopped");
            return S_OK;
        case StopResult::AlreadyStopped:
            Print(client, "windbgmcp: not running");
            return S_OK;
        case StopResult::Scheduled:
            Print(client, "windbgmcp: stopping asynchronously");
            return S_OK;
        case StopResult::Busy:
        default:
            Print(client, "windbgmcp: stop is busy; lifecycle state=%s",
                  windbgmcp::lifecycle::ToString(CurrentState(g)));
            return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
}

__declspec(dllexport) HRESULT CALLBACK
status(PDEBUG_CLIENT client, PCSTR /*args*/) {
    auto& g = G();
    std::lock_guard<std::mutex> lk(g.lifecycle_mu);
    if (g.lifecycle.State() != windbgmcp::lifecycle::SessionState::Running) {
        Print(client, "windbgmcp: lifecycle=%s. Use !mcpext.start to begin.",
              windbgmcp::lifecycle::ToString(g.lifecycle.State()));
        return S_OK;
    }
    Print(client,
          "windbgmcp: running\n"
          "  pipe       : %s\n"
          "  connected  : %s\n"
          "  event_sink : %s\n"
          "  engine_lane: serialized (request worker + callback actor)\n"
          "  generation : %llu\n"
          "  callback_tid: %lu",
          g.pipe ? g.pipe->Endpoint().c_str() : "<unknown>",
          g.pipe && g.pipe->IsConnected() ? "yes" : "no",
          g.sink ? g.sink->StateText() : "MISSING",
          static_cast<unsigned long long>(
              g.pipe ? g.pipe->CurrentGeneration() : 0),
          static_cast<unsigned long>(g.sink ? g.sink->OwnerThreadId() : 0));
    return S_OK;
}

__declspec(dllexport) HRESULT CALLBACK
help(PDEBUG_CLIENT client, PCSTR /*args*/) {
    Print(client,
          "mcpext - WinDbg <-> MCP bridge\n"
          "  !mcpext.start [pipe]  listen on an optional pipe name/endpoint\n"
          "  !mcpext.stop     close the pipe and stop the event sink\n"
          "  !mcpext.status   show pipe and connection status\n"
          "  !mcpext.help     this message\n"
          "pipe examples: windbgmcp-run-42 or \\\\.\\pipe\\windbgmcp-run-42");
    return S_OK;
}

} // extern "C"
