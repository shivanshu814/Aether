add_library(aether_reference INTERFACE)
add_library(aether::reference ALIAS aether_reference)
target_include_directories(aether_reference INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_reference INTERFACE aether::core)
