// SPDX-License-Identifier: MIT
#include "util/debug_client.h"

#include <mutex>

namespace windbgmcp::dbg {

namespace {
std::mutex g_mu;
CComPtr<IDebugClient> g_client;
std::mutex g_engine_mu;   // serialises KD-touching dbgeng calls
} // namespace

CComPtr<IDebugClient> Get() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_client) {
        CComPtr<IDebugClient> c;
        if (SUCCEEDED(::DebugCreate(__uuidof(IDebugClient), reinterpret_cast<void**>(&c)))) {
            g_client = c;
        }
    }
    return g_client;
}

void Reset() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_client.Release();
}

std::mutex& Lock() {
    return g_engine_mu;
}

} // namespace windbgmcp::dbg
