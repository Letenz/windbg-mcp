cmake_minimum_required(VERSION 3.20)

# Use CMAKE_MSVC_RUNTIME_LIBRARY abstraction (CMake 3.15+). Without NEW, the
# /MD vs /MT switch falls back to legacy per-flag patching in CMAKE_CXX_FLAGS.
cmake_policy(SET CMP0091 NEW)

project(windbg-mcp
    VERSION 2.0.0
    LANGUAGES CXX
    DESCRIPTION "Clean-room WinDbg <-> MCP bridge for AI-driven kernel debugging."
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT for all configurations.
#   Debug   -> /MTd   (libcmtd, ucrtd, vcruntimed, msvcprtd  — all static)
#   Release -> /MT    (libcmt,  ucrt,  vcruntime,  msvcprt   — all static)
# Why:
#   - mcpext.dll lives inside WinDbg.exe's process. Statically linking the
#     CRT keeps our heap, errno, locale, etc. independent of whatever CRT
#     WinDbg itself uses -- one less source of cross-DLL state corruption.
#   - windbg-mcp.exe ships as a single self-contained binary; no
#     dependency on the VC++ Redistributable on the operator's machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Release builds emit a PDB so we can resolve symbols in post-mortem dumps
# of our own binaries.
#
#   /Zi              (compiler)  produce program database
#   /DEBUG           (linker)    emit a .pdb next to the binary
#   /OPT:REF         (linker)    strip unreferenced functions/data
#   /OPT:ICF         (linker)    fold identical COMDATs
#   /PDBALTPATH:%_PDB% (linker)  embed PDB *basename only* in the binary,
#                                so the build-machine absolute path does
#                                not leak into shipped DLL/EXE
#
# /DEBUG defaults to disabling /OPT:REF and /OPT:ICF; we re-enable them
# explicitly so the released binary is still as small/optimised as without
# debug info.
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options(
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>
    )
endif()

if(NOT WIN32)
    message(FATAL_ERROR "windbg-mcp targets Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "windbg-mcp targets x64 only. Re-run cmake with -A x64.")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(WINDBGMCP_BUILD_TESTS     "Build C++ unit tests."                       ON)
option(WINDBGMCP_ENABLE_LOGGING  "Compile in OutputDebugString + on-disk logging. \
Off by default — production builds should not write to the operator's disk \
on every tool call. Turn on for development / triage." OFF)

if(WINDBGMCP_ENABLE_LOGGING)
    add_compile_definitions(WINDBGMCP_LOG_ENABLED=1)
    message(STATUS "windbg-mcp: logging is ENABLED (writes mcpext.log / windbg-mcp.log next to the binary)")
else()
    message(STATUS "windbg-mcp: logging is DISABLED (WMCP_LOG expands to no-op)")
endif()

# ---------------------------------------------------------------------------
# Third-party (nlohmann/json) via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
add_subdirectory(ext)     # windbgmcpExt.dll      -> WinDbg loads via .load
add_subdirectory(host)    # windbg-mcp.exe        -> AI client spawns via stdio

if(WINDBGMCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

