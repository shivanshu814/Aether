cmake_minimum_required(VERSION 3.25)
project(aether VERSION 0.1.0 LANGUAGES CXX)

# ---------- C++ standard ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- Options ----------
option(AETHER_BUILD_TESTS "Build tests" ON)
option(AETHER_NATIVE "Tune for the build machine (-mcpu/-march=native)" OFF)
set(AETHER_SANITIZER "" CACHE STRING "e.g. address,undefined  or  thread")

# ---------- Sanitizers ----------
if(AETHER_SANITIZER)
    add_compile_options(-fsanitize=${AETHER_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${AETHER_SANITIZER})
endif()

if(AETHER_NATIVE)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-mcpu=native AETHER_HAS_MCPU_NATIVE)
    if(AETHER_HAS_MCPU_NATIVE)
        add_compile_options(-mcpu=native)
    else()
        add_compile_options(-march=native)
    endif()
endif()

find_package(Threads REQUIRED)

# ---------- Warnings ----------
add_library(aether_warnings INTERFACE)
add_library(aether::warnings ALIAS aether_warnings)
target_compile_options(aether_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

# ---------- Code ----------
add_subdirectory(libs)
add_subdirectory(apps)

if(AETHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()