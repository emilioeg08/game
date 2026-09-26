# Synthetic workloads for tests, benchmarks and the headless runner. Not game content.
add_library(gx_scenarios STATIC
    SyntheticGalaxy.cpp
    SyntheticGalaxy.h
)

target_link_libraries(gx_scenarios PUBLIC gx_simulation)
gx_configure_target(gx_scenarios)
