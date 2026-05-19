# C++ tests, shared GoogleTest discovery.

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# Our project builds with the static CRT (/MT, /MTd) — gtest must match,
# otherwise the test exe ends up with two CRTs and the linker emits LNK4098
# (or worse, mixed-heap UB at runtime).
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_subdirectory(ext)
add_subdirectory(host)
