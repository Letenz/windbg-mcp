# ---------------------------------------------------------------------------
# mcpext.dll  - WinDbg extension (dbgeng client)
# ---------------------------------------------------------------------------
#
# This builds a DLL that exports !mcpext.start / .stop / .status. WinDbg
# loads it via `.load mcpext`. The DLL opens a named pipe and serves
# JSON requests from windbg-mcp.exe (the host).

add_library(mcpext SHARED
    src/dllmain.cpp
    src/exports.cpp
    src/ipc/frame_codec.cpp
    src/ipc/pipe_server.cpp
    src/ipc/router.cpp
    src/events/event_sink.cpp
    src/events/publisher.cpp
    src/events/event_history.cpp
    src/handlers/handlers_register.cpp
    src/handlers/session.cpp
    src/handlers/run_cmd.cpp
    src/handlers/wait_event.cpp
    src/handlers/break_in.cpp
    src/handlers/exit.cpp
    src/util/debug_client.cpp
    src/util/encoding.cpp
    src/util/log.cpp
    src/util/status.cpp
)

target_include_directories(mcpext
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(mcpext
    PRIVATE
        nlohmann_json::nlohmann_json
        dbgeng       # IDebugClient / IDebugControl / IDebugSymbols / ...
        dbghelp
)

# Module-definition file controls exported symbol names (start, stop, ...).
target_sources(mcpext PRIVATE src/mcpext.def)
set_target_properties(mcpext PROPERTIES
    OUTPUT_NAME "mcpext"
    PREFIX ""
)

if(MSVC)
    target_link_options(mcpext PRIVATE $<$<CONFIG:Release>:/PDBALTPATH:mcpext.pdb>)
    target_compile_options(mcpext PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    target_compile_definitions(mcpext PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

set_target_properties(mcpext PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         ${CMAKE_BINARY_DIR}/ext
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/ext/Release
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/ext/Debug
)
