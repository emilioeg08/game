add_library(gx_simulation STATIC
    Kernel/Simulation.cpp
    Kernel/Simulation.h
    Kernel/SystemScheduler.cpp
    Kernel/SystemScheduler.h
)

target_link_libraries(gx_simulation PUBLIC gx_engine)
gx_configure_target(gx_simulation)
