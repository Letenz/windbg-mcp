// SPDX-License-Identifier: MIT
#include "util/log.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>

namespace windbgmcp::log {

namespace {
std::atomic<Level> g_min{Level::Info};
std::mutex         g_file_mu;
FILE*              g_file = nullptr;
bool               g_file_tried = false;

const char* TagA(Level lvl) {
    switch (lvl) {
        case Level::Trace: return "trace";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "?";
}

const wchar_t* TagW(Level lvl) {
    switch (lvl) {
        case Level::Trace: return L"trace";
        case Level::Info:  return L"info";
        case Level::Warn:  return L"warn";
        case Level::Error: return L"error";
    }
    return L"?";
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                  static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(n));
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                          static_cast<int>(wide.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                  static_cast<int>(utf8.size()),
                                  nullptr, 0);
    if (n <= 0) return {};
    std::wstring out;
    out.resize(static_cast<std::size_t>(n));
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                          static_cast<int>(utf8.size()),
                          out.data(), n);
    return out;
}

FILE* GetFile() {
    std::lock_guard<std::mutex> lk(g_file_mu);
    if (g_file_tried) return g_file;
    g_file_tried = true;

    // Write next to our own DLL on disk. This sidesteps MSIX/AppContainer
    // redirection of %TEMP% (WinDbg Preview is packaged) and gives the
    // operator a single predictable location.
    HMODULE hSelf = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetFile), &hSelf)) {
        return nullptr;
    }
    wchar_t full[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(hSelf, full, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    // Strip filename → directory + L"mcpext.log".
    for (DWORD i = n; i > 0; --i) {
        if (full[i - 1] == L'\\' || full[i - 1] == L'/') {
            full[i] = L'\0';
            break;
        }
    }
    wchar_t path[MAX_PATH] = {0};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%smcpext.log", full);

    // Share-deny-write: others may open for read while we hold it open.
    g_file = _wfsopen(path, L"ab", _SH_DENYWR);
    if (g_file) {
        std::fputs("\n--- mcpext.dll session begin pid=", g_file);
        std::fprintf(g_file, "%lu ---\n",
                     static_cast<unsigned long>(::GetCurrentProcessId()));
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

void Set_min_level(Level lvl) {
    g_min.store(lvl, std::memory_order_relaxed);
}

void Write(Level lvl, std::wstring_view line) {
    if (static_cast<int>(lvl) < static_cast<int>(g_min.load(std::memory_order_relaxed))) {
        return;
    }
    // OutputDebugString (wide variant) — cheap.
    {
        std::wstring buf;
        buf.reserve(line.size() + 64);
        buf.append(L"[windbgmcp/");
        buf.append(TagW(lvl));
        buf.append(L"] ");
        buf.append(line);
        buf.push_back(L'\n');
        ::OutputDebugStringW(buf.c_str());
    }
    // File — single UTF-8 line with timestamp + tid prefix.
    if (FILE* f = GetFile()) {
        char ts[32];
        FormatTimestamp(ts, sizeof(ts));
        const std::string body = WideToUtf8(line);
        std::lock_guard<std::mutex> lk(g_file_mu);
        std::fprintf(f, "[%s][tid=%lu][windbgmcp/%s] %s\n",
                     ts, static_cast<unsigned long>(::GetCurrentThreadId()),
                     TagA(lvl), body.c_str());
        std::fflush(f);
    }
}

void Write(Level lvl, std::string_view line) {
    Write(lvl, Widen(line));
}

} // namespace windbgmcp::log
