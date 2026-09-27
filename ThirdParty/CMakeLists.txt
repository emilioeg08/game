# Third-party dependencies of the graphical client, pinned by version and SHA-256 (ADR-016).
# Only the client links them: the engine, the simulation and the tests stay dependency-free.
include(FetchContent)

# SDL3: window, input and the 2D renderer, built as a static library with only what the client needs.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)

FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.tar.gz
    URL_HASH SHA256=c2ee715e42ec520c4d11fd8d249ef1d2b2baf4ad31148b72b3b000276c0b3633
    SYSTEM
)

# Dear ImGui: immediate-mode UI for panels, inspectors and the map overlay.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    SYSTEM
)

FetchContent_MakeAvailable(sdl3 imgui)

# Their licenses travel with the game (zlib and MIT: the notice must be included).
install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME SDL3.txt COMPONENT client)
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME DearImGui.txt COMPONENT client)

add_library(gx_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(gx_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(gx_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(gx_imgui PUBLIC SDL3::SDL3)
