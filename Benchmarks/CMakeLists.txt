add_executable(gx_bench
    BenchCommon.h
    BenchEngine.cpp
    BenchEntities.cpp
    BenchSimulation.cpp
    main.cpp
)
target_link_libraries(gx_bench PRIVATE gx_scenarios gx_gameplay)
gx_configure_target(gx_bench)
gx_add_build_info(gx_bench)
