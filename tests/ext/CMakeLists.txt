add_executable(ext_unittests
    frame_codec_test.cpp
    event_history_test.cpp
    connection_fence_test.cpp
    session_state_test.cpp
    command_guard_test.cpp
    thread_affinity_test.cpp
    pipe_server_generation_test.cpp
    publisher_cancel_test.cpp
    event_sink_lifecycle_test.cpp
    router_lifecycle_test.cpp
    unload_barrier_test.cpp
    lifecycle_handler_test.cpp
)

target_link_libraries(ext_unittests
    PRIVATE
        gtest_main
        nlohmann_json::nlohmann_json
)

# Pull the tested .cpp files in directly. Router lifecycle coverage links the
# small dbgeng client utility, but still avoids loading the extension exports
# or requiring a live debugger session.
target_sources(ext_unittests PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/src/ipc/frame_codec.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/ipc/pipe_server.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/ipc/router.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/events/event_history.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/events/event_sink.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/events/publisher.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/handlers/exit.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/util/debug_client.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/util/log.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/util/status.cpp
)
target_include_directories(ext_unittests PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/src
    ${CMAKE_SOURCE_DIR}/ext/include
)

target_link_libraries(ext_unittests PRIVATE advapi32 dbgeng)

gtest_discover_tests(ext_unittests)

add_executable(extension_export_probe extension_export_probe.cpp)
add_dependencies(extension_export_probe mcpext)
add_test(NAME ExtensionExports.CanUnload
         COMMAND extension_export_probe "$<TARGET_FILE:mcpext>")
