add_library(gx_simulation STATIC
    Commands/CommandQueue.cpp
    Commands/CommandQueue.h
    Economy/Economy.cpp
    Economy/Economy.h
    Economy/Finance.cpp
    Economy/Finance.h
    Events/EventBus.cpp
    Events/EventBus.h
    Kernel/Simulation.cpp
    Kernel/Simulation.h
    Kernel/SystemScheduler.cpp
    Kernel/SystemScheduler.h
    World/ComponentStore.h
    World/EntityRegistry.cpp
    World/EntityRegistry.h
    World/Inspect.h
    World/World.cpp
    World/World.h
)

target_link_libraries(gx_simulation PUBLIC gx_engine)
gx_configure_target(gx_simulation)
