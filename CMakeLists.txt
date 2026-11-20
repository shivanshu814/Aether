cmake_minimum_required(VERSION 3.25)
project(aether VERSION 0.1.0 LANGUAGES CXX)

# ---------- C++ standard ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- Options ----------
option(AETHER_BUILD_TESTS "Build tests" ON)
set(AETHER_SANITIZER "" CACHE STRING "e.g. address,undefined  or  thread")

# ---------- Sanitizers ----------
if(AETHER_SANITIZER)
    add_compile_options(-fsanitize=${AETHER_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${AETHER_SANITIZER})
endif()

# ---------- Warnings ----------
add_library(aether_warnings INTERFACE)
add_library(aether::warnings ALIAS aether_warnings)
target_compile_options(aether_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

# ---------- Code ----------
add_subdirectory(libs)

if(AETHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()