add_executable(gx_headless Headless/main.cpp)
target_link_libraries(gx_headless PRIVATE gx_scenarios)
gx_configure_target(gx_headless)
gx_add_build_info(gx_headless)

if(GX_BUILD_CLIENT)
    add_executable(gx_game
        Game/GameApp.cpp
        Game/GameApp.h
        Game/InspectorView.cpp
        Game/InspectorView.h
        Game/MapView.cpp
        Game/MapView.h
        Game/main.cpp
    )
    target_link_libraries(gx_game PRIVATE gx_gameplay gx_imgui)
    gx_configure_target(gx_game)
    gx_add_build_info(gx_game)
endif()
