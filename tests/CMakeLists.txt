include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(unit_tests
    unit/core/test_core.cpp
)
target_link_libraries(unit_tests PRIVATE
    aether::core
    aether::warnings
    GTest::gtest_main
)
gtest_discover_tests(unit_tests)