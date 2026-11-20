include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(unit_tests
    unit/core/test_core.cpp
    unit/core/test_event_sink.cpp
    unit/memory/test_pool.cpp
    unit/concurrency/test_spsc_ring.cpp
    unit/book/test_book_parts.cpp
    unit/engine/test_engine.cpp
    scenario/test_matching.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(unit_tests PRIVATE
    aether::core
    aether::memory
    aether::reference
    aether::book
    aether::concurrency
    aether::journal
    aether::engine
    aether::warnings
    GTest::gtest_main
)
gtest_discover_tests(unit_tests)
