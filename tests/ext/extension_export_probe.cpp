// SPDX-License-Identifier: MIT

#include <Windows.h>

#include <cwchar>

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) return 2;
    HMODULE module = ::LoadLibraryW(argv[1]);
    if (!module) {
        ::fwprintf(stderr, L"LoadLibraryW failed for %ls (gle=%lu)\n",
                   argv[1], static_cast<unsigned long>(::GetLastError()));
        return 3;
    }

    using CanUnload = HRESULT (CALLBACK*)();
    const auto can_unload = reinterpret_cast<CanUnload>(
        ::GetProcAddress(module, "DebugExtensionCanUnload"));
    const bool found = can_unload != nullptr;
    const bool quiescent = found && can_unload() == S_OK;
    ::FreeLibrary(module);
    return found && quiescent ? 0 : 4;
}
