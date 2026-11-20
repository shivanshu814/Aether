add_library(aether_sim INTERFACE)
add_library(aether::sim ALIAS aether_sim)
target_include_directories(aether_sim INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_sim INTERFACE aether::core aether::engine aether::reference)
