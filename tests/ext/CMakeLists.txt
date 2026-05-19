add_executable(ext_unittests
    frame_codec_test.cpp
    event_history_test.cpp
)

target_link_libraries(ext_unittests
    PRIVATE
        gtest_main
        nlohmann_json::nlohmann_json
)

# Pull the tested .cpp files in directly so we don't need to link the
# whole DLL (which would drag dbgeng/dbghelp into the test binary).
target_sources(ext_unittests PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/src/ipc/frame_codec.cpp
    ${CMAKE_SOURCE_DIR}/ext/src/events/event_history.cpp
)
target_include_directories(ext_unittests PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/src
    ${CMAKE_SOURCE_DIR}/ext/include
)

gtest_discover_tests(ext_unittests)
