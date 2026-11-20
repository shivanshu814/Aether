add_library(aether_journal INTERFACE)
add_library(aether::journal ALIAS aether_journal)
target_include_directories(aether_journal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_journal INTERFACE aether::core)
