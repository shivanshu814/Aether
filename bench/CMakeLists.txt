# The benchmark is its own binary because it replaces global operator new to count
# allocations. That must never be linked into the engine or the tests.
add_executable(aether_bench main.cpp alloc_counter.cpp)
target_link_libraries(aether_bench PRIVATE
    aether::book aether::reference aether::engine aether::runtime aether::sim aether::warnings)
