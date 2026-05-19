# ---------------------------------------------------------------------------
# windbg-mcp.exe  - MCP server (speaks JSON-RPC over stdio).
# ---------------------------------------------------------------------------
#
# Spawned by the AI client (Claude Code / Cursor / etc.). Talks two protocols:
#
#   stdin / stdout:           MCP JSON-RPC (with the AI client)
#   \\.\pipe\windbgmcp:       our own length-prefixed JSON (with windbgmcpExt.dll)
#
# Zero runtime dependencies besides Windows itself.

add_executable(windbg-mcp WIN32
    src/main.cpp
    src/transport/pipe_client.cpp
    src/transport/frame_codec.cpp
    src/mcp/stdio_io.cpp
    src/mcp/jsonrpc_server.cpp
    src/tools/dispatcher.cpp
    src/tools/run_cmd.cpp
    src/tools/analyze_crash.cpp
    src/analysis/bugcheck_parser.cpp
    src/analysis/stack_parser.cpp
    src/util/log.cpp
    src/util/path.cpp
)

# Override WIN32 above: we want a console app (the MCP transport is stdio).
set_target_properties(windbg-mcp PROPERTIES
    WIN32_EXECUTABLE                FALSE
    OUTPUT_NAME                     "windbg-mcp"
    RUNTIME_OUTPUT_DIRECTORY        ${CMAKE_BINARY_DIR}/host
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/host/Release
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/host/Debug
)

target_include_directories(windbg-mcp
    PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/ext/include    # for windbgmcp/protocol.h
)

target_link_libraries(windbg-mcp
    PRIVATE
        nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(windbg-mcp PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    target_compile_definitions(windbg-mcp PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
