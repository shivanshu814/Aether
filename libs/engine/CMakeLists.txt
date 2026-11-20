add_library(aether_engine INTERFACE)
add_library(aether::engine ALIAS aether_engine)
target_include_directories(aether_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_engine INTERFACE aether::core)
