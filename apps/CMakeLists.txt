add_executable(aether_replay replay/main.cpp)
target_link_libraries(aether_replay PRIVATE
    aether::book aether::reference aether::runtime aether::warnings)

add_executable(aetherd aetherd/main.cpp)
target_link_libraries(aetherd PRIVATE
    aether::book aether::reference aether::runtime aether::sim aether::warnings)
