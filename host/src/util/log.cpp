// SPDX-License-Identifier: MIT
#include "util/log.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <share.h>
#include <string>

namespace wmh::log {

namespace {
std::atomic<Level> g_min{Level::Info};
std::mutex         g_file_mu;
FILE*              g_file = nullptr;
bool               g_file_tried = false;

const char* Tag(Level lvl) {
    switch (lvl) {
        case Level::Trace: return "trace";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "?";
}

// Lazily open <exe-dir>\windbg-mcp.log in append mode. Returns nullptr
// on failure (we just skip file logging — OutputDebugString still goes).
FILE* GetFile() {
    std::lock_guard<std::mutex> lk(g_file_mu);
    if (g_file_tried) return g_file;
    g_file_tried = true;

    // Write next to windbg-mcp.exe on disk so the operator has a single
    // predictable location, independent of MCP client cwd or %TEMP%.
    wchar_t full[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, full, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    for (DWORD i = n; i > 0; --i) {
        if (full[i - 1] == L'\\' || full[i - 1] == L'/') {
            full[i] = L'\0';
            break;
        }
    }
    wchar_t path[MAX_PATH] = {0};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"%swindbg-mcp.log", full);
    // Share-deny-write: others may open for read while we hold it open.
    g_file = _wfsopen(path, L"ab", _SH_DENYWR);
    if (g_file) {
        std::fputs("\n--- windbg-mcp.exe session begin pid=", g_file);
        std::fprintf(g_file, "%lu ---\n", static_cast<unsigned long>(::GetCurrentProcessId()));
        std::fflush(g_file);
    }
    return g_file;
}

void FormatTimestamp(char* buf, std::size_t n) {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    _snprintf_s(buf, n, _TRUNCATE,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}
} // namespace

void SetMinLevel(Level lvl) {
    g_min.store(lvl, std::memory_order_relaxed);
}

void Write(Level lvl, std::string_view line) {
    if (static_cast<int>(lvl) < static_cast<int>(g_min.load(std::memory_order_relaxed))) {
        return;
    }
    char ts[32];
    FormatTimestamp(ts, sizeof(ts));
    const DWORD tid = ::GetCurrentThreadId();

    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "[%s][tid=%lu][windbg-mcp/%s] %.*s\n",
                  ts, static_cast<unsigned long>(tid), Tag(lvl),
                  static_cast<int>(line.size()), line.data());

    // 1) DebugView (cheap; goes nowhere if nobody is listening).
    ::OutputDebugStringA(buf);

    // 2) File. Mutex serialises writes across threads.
    if (FILE* f = GetFile()) {
        std::lock_guard<std::mutex> lk(g_file_mu);
        std::fputs(buf, f);
        std::fflush(f);
    }
}

} // namespace wmh::log
