// SPDX-License-Identifier: MIT
//
// Standard DLL entry point. WinDbg loads this DLL via LoadLibrary, then
// invokes DebugExtensionInitialize (defined in exports.cpp) to bootstrap.
// We have nothing to do at DllMain time.

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*lp*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
