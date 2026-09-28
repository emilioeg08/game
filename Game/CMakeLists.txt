# Game layer: playable scenarios, provisional content and the presentation snapshot.
# No graphics dependency: everything here runs headless (tests) as well as under the graphical client.
add_library(gx_gameplay STATIC
    Presentation/SystemSnapshot.cpp
    Presentation/SystemSnapshot.h
    Presentation/UserSettings.cpp
    Presentation/UserSettings.h
    Sandbox/Content.h
    Sandbox/Contracts.h
    Sandbox/Fleet.h
    Sandbox/Sandbox.cpp
    Sandbox/Sandbox.h
    Sandbox/SandboxContracts.cpp
    Sandbox/SandboxDetail.h
    Sandbox/SandboxFinance.cpp
    Sandbox/SandboxFleet.cpp
)

target_link_libraries(gx_gameplay PUBLIC gx_space)
gx_configure_target(gx_gameplay)
