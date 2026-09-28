add_executable(gx_tests
    TestFramework.h
    TestMain.cpp
    TestCombat.cpp
    TestCommands.cpp
    TestEconomy.cpp
    TestCore.cpp
    TestDestruction.cpp
    TestDeterminism.cpp
    TestEvents.cpp
    TestFleet.cpp
    TestJobs.cpp
    TestKernel.cpp
    TestMemoryMath.cpp
    TestProfiling.cpp
    TestSandbox.cpp
    TestSaveLoad.cpp
    TestSensors.cpp
    TestSerialization.cpp
    TestSpace.cpp
    TestText.cpp
    TestTime.cpp
    TestWorld.cpp
)

target_link_libraries(gx_tests PRIVATE gx_scenarios gx_gameplay)
gx_configure_target(gx_tests)
target_compile_definitions(gx_tests PRIVATE GX_SOURCE_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

# One CTest entry per suite. gx_tests fails when a suite matches no test, so this list cannot silently drift.
set(GX_TEST_SUITES Core Memory Math Jobs Time Profiling Serialization World Events Commands Kernel SaveLoad
    Determinism Space Sandbox Sensors Combat Economy Text Fleet Destruction)
foreach(suite IN LISTS GX_TEST_SUITES)
    add_test(NAME ${suite} COMMAND gx_tests --suite ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
endforeach()
