// SPDX-License-Identifier: MIT
//
// IDebugEventCallbacks implementation. !mcpext.start launches a dedicated
// owner actor asynchronously; after the bang command returns, that actor
// creates its own thread-bound client and registers callbacks on it. Every
// callback marshals the relevant fields into a windbgmcp::events::Event and
// hands it to the Publisher; all callbacks return DEBUG_STATUS_NO_CHANGE so
// they never alter execution status (so WinDbg's UI behaviour is intact).
//
// Bugcheck detection: the kernel triggers KeBugCheckEx, which dbgeng
// surfaces as an Exception callback with code STATUS_BREAKPOINT and the
// first ExceptionInformation slot set to the bugcheck code. We also handle
// the case where dbgeng has already torn down to a bugcheck state at
// connect time, in which case the bugcheck info is queryable via
// IDebugControl::ReadBugCheckData.

#pragma once

#include "lifecycle/thread_affinity.h"

#include <DbgEng.h>
#include <atlbase.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace windbgmcp::events {

class EventSink : public IDebugEventCallbacks {
public:
    using ClientFactory = std::function<HRESULT(IDebugClient**)>;

    enum class InstallState {
        NotStarted,
        Installing,
        Installed,
        Failed,
        Stopped,
    };

    // client_factory is a test seam. Production callers leave it empty and
    // the callback-owner thread creates its own client with DebugCreate.
    static EventSink* Create(ClientFactory client_factory = {});

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, PVOID* iface) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IDebugEventCallbacks
    STDMETHOD(GetInterestMask)(PULONG mask) override;
    STDMETHOD(Breakpoint)(PDEBUG_BREAKPOINT bp) override;
    STDMETHOD(Exception)(PEXCEPTION_RECORD64 ex, ULONG first_chance) override;
    STDMETHOD(CreateThread)(ULONG64 handle, ULONG64 data_offset, ULONG64 start_offset) override;
    STDMETHOD(ExitThread)(ULONG exit_code) override;
    STDMETHOD(CreateProcess)(ULONG64 image_handle, ULONG64 handle, ULONG64 base_offset,
                             ULONG mod_size, PCSTR mod_name, PCSTR image_name,
                             ULONG checksum, ULONG ts, ULONG64 init_thread_handle,
                             ULONG64 thread_data_offset, ULONG64 start_offset) override;
    STDMETHOD(ExitProcess)(ULONG exit_code) override;
    STDMETHOD(LoadModule)(ULONG64 image_handle, ULONG64 base_offset, ULONG mod_size,
                          PCSTR mod_name, PCSTR image_name, ULONG checksum,
                          ULONG ts) override;
    STDMETHOD(UnloadModule)(PCSTR image_base_name, ULONG64 base_offset) override;
    STDMETHOD(SystemError)(ULONG error, ULONG level) override;
    STDMETHOD(SessionStatus)(ULONG status) override;
    STDMETHOD(ChangeDebuggeeState)(ULONG flags, ULONG64 argument) override;
    STDMETHOD(ChangeEngineState)(ULONG flags, ULONG64 argument) override;
    STDMETHOD(ChangeSymbolState)(ULONG flags, ULONG64 argument) override;

    // Start a dedicated callback-owner thread and return as soon as the
    // thread has been created. Initialization must remain asynchronous:
    // !mcpext.start executes while WinDbg owns an internal engine lock, so
    // waiting here for DebugCreate/SetEventCallbacks would deadlock with the
    // new thread. The actor creates, uses, and releases its client itself.
    bool InstallAsync();
    InstallState State() const noexcept { return m_install_state.load(); }
    const char* StateText() const noexcept;

    // Signal and join the callback-owner thread. SetEventCallbacks(nullptr)
    // and IDebugClient::Release both execute inside that owner thread.
    bool Uninstall();
    bool IsOwnerThread() const noexcept;
    DWORD OwnerThreadId() const noexcept { return m_owner.OwnerThreadId(); }

private:
    explicit EventSink(ClientFactory client_factory)
        : m_client_factory(std::move(client_factory)) {}
    ~EventSink() = default;
    void OwnerLoop();

    std::atomic<ULONG> m_ref{1};
    // Only the owner actor accesses m_client.
    CComPtr<IDebugClient> m_client;
    ClientFactory m_client_factory;
    lifecycle::ThreadAffinity m_owner;
    std::thread m_owner_thread;
    std::mutex m_owner_mu;
    std::condition_variable m_owner_cv;
    std::atomic<InstallState> m_install_state{InstallState::NotStarted};
    bool m_stop_requested = false;
    bool m_uninstall_ok = false;

    // Last seen exec status, used by ChangeEngineState to compute "from".
    std::atomic<ULONG> m_last_status{DEBUG_STATUS_NO_CHANGE};

    // True between an Exception(bugcheck) callback and the next
    // ChangeEngineState transition; used to suppress the regular `break`
    // event in favor of the `bugcheck` one.
    std::atomic<bool>  m_pending_bugcheck{false};

    // Most recent breakpoint id, set in Breakpoint() so the next break
    // event can carry it as `reason="breakpoint"`.
    std::atomic<ULONG> m_last_bp_id{0};
};

} // namespace windbgmcp::events
