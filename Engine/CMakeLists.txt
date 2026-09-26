add_library(gx_engine STATIC
    Core/Assert.cpp
    Core/Assert.h
    Core/Hash.h
    Core/Log.cpp
    Core/Log.h
    Core/Platform.cpp
    Core/Platform.h
    Core/Random.h
    Core/Types.h
    Jobs/JobSystem.cpp
    Jobs/JobSystem.h
    Math/Vec3.h
    Memory/LinearArena.cpp
    Memory/LinearArena.h
    Profiling/MemoryUsage.cpp
    Profiling/MemoryUsage.h
    Profiling/Profiler.cpp
    Profiling/Profiler.h
    Profiling/Statistics.cpp
    Profiling/Statistics.h
    Time/SimClock.h
    Time/SimTime.cpp
    Time/SimTime.h
    Time/Stopwatch.h
    Time/TimeController.cpp
    Time/TimeController.h
)

target_include_directories(gx_engine PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(gx_engine PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(gx_engine PRIVATE psapi)
endif()

# Public: every translation unit that sees engine headers must agree on these.
target_compile_definitions(gx_engine PUBLIC
    GX_ENABLE_ASSERTS=$<IF:$<OR:$<CONFIG:Debug>,$<BOOL:${GX_FORCE_ASSERTS}>>,1,0>
    GX_ENABLE_PROFILING=$<BOOL:${GX_ENABLE_PROFILING}>
)

gx_configure_target(gx_engine)
