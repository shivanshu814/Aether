add_library(aether_core INTERFACE)
add_library(aether::core ALIAS aether_core)

target_include_directories(aether_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_core INTERFACE cxx_std_20)