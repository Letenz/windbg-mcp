add_executable(host_unittests
    bugcheck_parser_test.cpp
    stack_parser_test.cpp
    frame_codec_test.cpp
)

target_link_libraries(host_unittests
    PRIVATE
        gtest_main
        nlohmann_json::nlohmann_json
)

target_sources(host_unittests PRIVATE
    ${CMAKE_SOURCE_DIR}/host/src/analysis/bugcheck_parser.cpp
    ${CMAKE_SOURCE_DIR}/host/src/analysis/stack_parser.cpp
    ${CMAKE_SOURCE_DIR}/host/src/transport/frame_codec.cpp
)
target_include_directories(host_unittests PRIVATE
    ${CMAKE_SOURCE_DIR}/host/src
    ${CMAKE_SOURCE_DIR}/ext/include
)

gtest_discover_tests(host_unittests)
