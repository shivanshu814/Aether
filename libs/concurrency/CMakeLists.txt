add_library(aether_concurrency INTERFACE)
add_library(aether::concurrency ALIAS aether_concurrency)
target_include_directories(aether_concurrency INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
