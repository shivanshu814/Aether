add_library(aether_runtime INTERFACE)
add_library(aether::runtime ALIAS aether_runtime)
target_include_directories(aether_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_runtime INTERFACE aether::engine aether::journal aether::concurrency Threads::Threads)
