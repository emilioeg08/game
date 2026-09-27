add_executable(gx_tests
    TestFramework.h
    TestMain.cpp
    TestCombat.cpp
    TestCommands.cpp
    TestCore.cpp
    TestDeterminism.cpp
    TestEvents.cpp
    TestJobs.cpp
    TestKernel.cpp
    TestMemoryMath.cpp
    TestProfiling.cpp
    TestSandbox.cpp
    TestSaveLoad.cpp
    TestSensors.cpp
    TestSerialization.cpp
    TestSpace.cpp
    TestTime.cpp
    TestWorld.cpp
)

target_link_libraries(gx_tests PRIVATE gx_scenarios gx_gameplay)
gx_configure_target(gx_tests)

# One CTest entry per suite. gx_tests fails when a suite matches no test, so this list cannot silently drift.
set(GX_TEST_SUITES Core Memory Math Jobs Time Profiling Serialization World Events Commands Kernel SaveLoad
    Determinism Space Sandbox Sensors Combat)
foreach(suite IN LISTS GX_TEST_SUITES)
    add_test(NAME ${suite} COMMAND gx_tests --suite ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
endforeach()
