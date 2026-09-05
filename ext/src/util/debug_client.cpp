// SPDX-License-Identifier: MIT
#include "util/debug_client.h"

#include <mutex>
#include <thread>

namespace windbgmcp::dbg {

namespace {
std::mutex g_mu;
CComPtr<IDebugClient> g_client;
std::thread::id g_owner;
std::mutex g_engine_mu;   // serialises KD-touching dbgeng calls
} // namespace

CComPtr<IDebugClient> Get() {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto self = std::this_thread::get_id();
    if (g_client && g_owner != self) {
        // IDebugClient is thread-affine. Returning nullptr produces a
        // structured engine_error instead of invoking dbgeng from the wrong
        // thread if a future code path bypasses the serial router lane.
        return nullptr;
    }
    if (!g_client) {
        CComPtr<IDebugClient> c;
        // Client creation enters dbgeng too. Serialize it with the callback
        // actor's initialization/pump and all KD-touching handler calls.
        std::lock_guard<std::mutex> engine_lk(g_engine_mu);
        if (SUCCEEDED(::DebugCreate(__uuidof(IDebugClient), reinterpret_cast<void**>(&c)))) {
            g_client = c;
            g_owner = self;
        }
    }
    return g_client;
}

void Reset() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_client && g_owner != std::this_thread::get_id()) return;
    std::lock_guard<std::mutex> engine_lk(g_engine_mu);
    g_client.Release();
    g_owner = {};
}

std::mutex& Lock() {
    return g_engine_mu;
}

} // namespace windbgmcp::dbg
