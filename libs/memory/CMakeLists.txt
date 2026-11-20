add_library(aether_memory INTERFACE)
add_library(aether::memory ALIAS aether_memory)
target_include_directories(aether_memory INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
