# Game layer: playable scenarios, provisional content and the presentation snapshot.
# No graphics dependency: everything here runs headless (tests) as well as under the graphical client.
add_library(gx_gameplay STATIC
    Presentation/SystemSnapshot.cpp
    Presentation/SystemSnapshot.h
    Sandbox/Content.h
    Sandbox/Contracts.h
    Sandbox/Sandbox.cpp
    Sandbox/Sandbox.h
    Sandbox/SandboxContracts.cpp
)

target_link_libraries(gx_gameplay PUBLIC gx_space)
gx_configure_target(gx_gameplay)
