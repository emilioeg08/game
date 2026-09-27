add_executable(gx_headless Headless/main.cpp)
target_link_libraries(gx_headless PRIVATE gx_scenarios gx_gameplay)
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
        Game/Menus.cpp
        Game/main.cpp
    )
    target_link_libraries(gx_game PRIVATE gx_gameplay gx_imgui)
    gx_configure_target(gx_game)
    gx_add_build_info(gx_game)

    if(WIN32)
        # Shipping on Windows (ADR-034): icon, version information and manifest; a GUI program (no console
        # window) unless GX_CLIENT_CONSOLE, which the debug preset turns on.
        set(GX_GAME_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Game/resources/gx_game.ico")
        configure_file(Game/resources/gx_game.rc.in ${CMAKE_CURRENT_BINARY_DIR}/gx_game.rc @ONLY)
        target_sources(gx_game PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gx_game.rc Game/resources/gx_game.manifest)
        if(NOT GX_CLIENT_CONSOLE)
            set_target_properties(gx_game PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
    endif()

    # What players get: `cmake --install <build> --component client --prefix <folder>` is the Steam depot
    # (scripts/package.ps1). The program's symbols go to a separate component, never to players.
    install(TARGETS gx_game RUNTIME DESTINATION . COMPONENT client)
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:gx_game> DESTINATION . COMPONENT symbols OPTIONAL)
    endif()
endif()
