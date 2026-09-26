add_executable(gx_headless Headless/main.cpp)
target_link_libraries(gx_headless PRIVATE gx_scenarios)
gx_configure_target(gx_headless)
gx_add_build_info(gx_headless)
