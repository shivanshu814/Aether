add_library(aether_book INTERFACE)
add_library(aether::book ALIAS aether_book)
target_include_directories(aether_book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_book INTERFACE aether::core aether::memory)
