add_executable(gx_bench main.cpp)
target_link_libraries(gx_bench PRIVATE gx_scenarios)
gx_configure_target(gx_bench)
gx_add_build_info(gx_bench)
