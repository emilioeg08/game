// Main menu, pause menu and options (ADR-035). Menus are ImGui windows over the map: the main menu over a
// system running in the background, the pause menu over the stopped game with the HUD hidden.
#include "Apps/Game/GameApp.h"

#include "Engine/Core/Paths.h"
#include "Engine/Serialization/SaveFile.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <format>

namespace gx {
namespace {

constexpr const char* kTitle = "GALAXYENGINE"; // provisional name (docs/STEAM.md)
constexpr u64 kMaxSeed = 999'999'999;
constexpr f64 kMenuMetersPerPixel = 4e8; // the whole inner system on screen

// A borderless window, `width` wide (before the UI scale), centred on a point of the screen given as
// fractions of its size.
bool beginCentered(const char* name, float width, float scale, float x = 0.5f, float y = 0.5f) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * x, display.y * y}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({width * scale, 0.0f}, {width * scale, FLT_MAX});
    return ImGui::Begin(name, nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
}

bool menuButton(const char* label, float scale) {
    return ImGui::Button(label, {-FLT_MIN, 36.0f * scale});
}

void centeredText(const char* text, ImU32 color) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

} // namespace

bool GameApp::menuOpen() const {
    return m_screen == Screen::MainMenu || m_pauseMenu || m_showOptions || m_confirm != Confirm::None;
}

void GameApp::enterMainMenu() {
    m_screen = Screen::MainMenu;
    m_pauseMenu = false;
    m_confirm = Confirm::None;
    m_seedPanel = false;
    m_selected = {};
    m_selectedContact = 0;
    // The background: the whole system, turning at x10.
    m_map.camera().follow = simulation().world().components<CelestialBody>().entities().empty()
                                ? EntityId{}
                                : simulation().world().components<CelestialBody>().entities()[0];
    m_map.camera().metersPerPixel = kMenuMetersPerPixel;
    m_speedIndex = 2;
    m_paused = false;
    m_status.clear();
    resetTimeController();
    refreshSaveSummary();
}

void GameApp::startPlaying() {
    m_screen = Screen::Playing;
    m_pauseMenu = false;
    m_showOptions = false;
    m_confirm = Confirm::None;
    m_speedIndex = 0; // x1: real time
    m_paused = false;
    m_showHelp = m_settings.showHelpOnStart;
    m_map.camera().follow = sandbox().playerShip();
    m_map.camera().metersPerPixel = 30'000.0;
    resetTimeController();
}

void GameApp::refreshSaveSummary() {
    SaveFileContents contents;
    std::string error;
    m_saveSummary = readSaveFile(m_dataDir / "saves" / "quicksave.gxsave", contents, error)
                        ? contents.info.description
                        : std::string{};
}

void GameApp::handleMenuKeyboard() {
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) ||
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::GetIO().KeyAlt)) {
        toggleFullscreen();
    }
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        return;
    }
    // Esc backs out one level.
    if (m_confirm != Confirm::None) {
        m_confirm = Confirm::None;
    } else if (m_showOptions) {
        m_showOptions = false;
    } else if (m_seedPanel) {
        m_seedPanel = false;
    } else if (m_pauseMenu) {
        m_pauseMenu = false;
    }
}

void GameApp::drawMainMenu() {
    const float scale = m_displayScale * m_settings.uiScale;
    if (m_showOptions) {
        return; // the options take its place
    }
    const bool blocked = m_confirm != Confirm::None;
    // Left of centre: the running system stays visible.
    if (beginCentered("##mainmenu", 380.0f, scale, 0.27f, 0.5f)) {
        ImGui::BeginDisabled(blocked);
        ImGui::Dummy({0.0f, 8.0f * scale});
        ImGui::PushFont(nullptr, 44.0f);
        centeredText(kTitle, IM_COL32(235, 240, 255, 255));
        ImGui::PopFont();
        centeredText(std::format("Sandbox · versión {}", GX_VERSION).c_str(), IM_COL32(140, 160, 190, 255));
        ImGui::Dummy({0.0f, 12.0f * scale});

        if (!m_saveSummary.empty()) {
            if (menuButton("Continuar", scale)) {
                m_pending = PendingAction::Load;
            }
            ImGui::TextDisabled("%s", m_saveSummary.c_str());
            ImGui::Dummy({0.0f, 4.0f * scale});
        }
        if (menuButton("Nueva partida", scale)) {
            m_seedPanel = !m_seedPanel;
        }
        if (m_seedPanel) {
            ImGui::Indent(12.0f * scale);
            ImGui::TextDisabled("Cada semilla genera un sistema estelar distinto.");
            ImGui::SetNextItemWidth(160.0f * scale);
            ImGui::InputScalar("Semilla", ImGuiDataType_U64, &m_menuSeed);
            m_menuSeed = std::min(m_menuSeed, kMaxSeed);
            if (ImGui::Button("Empezar")) {
                m_newGameSeed = m_menuSeed;
                m_pending = PendingAction::NewGame;
            }
            ImGui::SameLine();
            if (ImGui::Button("Sistema al azar")) {
                m_newGameSeed.reset();
                m_pending = PendingAction::NewGame;
            }
            ImGui::Unindent(12.0f * scale);
            ImGui::Dummy({0.0f, 4.0f * scale});
        }
        if (menuButton("Opciones", scale)) {
            m_showOptions = true;
        }
        if (menuButton("Salir", scale)) {
            m_quit = true;
        }
        ImGui::Dummy({0.0f, 6.0f * scale});
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void GameApp::drawPauseMenu() {
    const float scale = m_displayScale * m_settings.uiScale;
    if (m_showOptions) {
        return;
    }
    const bool blocked = m_confirm != Confirm::None;
    if (beginCentered("##pausemenu", 320.0f, scale)) {
        ImGui::BeginDisabled(blocked);
        ImGui::Dummy({0.0f, 4.0f * scale});
        ImGui::PushFont(nullptr, 28.0f);
        centeredText("Pausa", IM_COL32(235, 240, 255, 255));
        ImGui::PopFont();
        ImGui::Dummy({0.0f, 8.0f * scale});
        if (menuButton("Continuar (Esc)", scale)) {
            m_pauseMenu = false;
        }
        if (menuButton("Guardar (F5)", scale)) {
            m_pending = PendingAction::Save;
            m_pauseMenu = false;
        }
        if (menuButton("Cargar (F9)", scale)) {
            m_pending = PendingAction::Load;
        }
        if (menuButton("Opciones", scale)) {
            m_showOptions = true;
        }
        if (menuButton("Menú principal", scale)) {
            m_confirm = Confirm::MainMenu;
        }
        if (menuButton("Salir al escritorio", scale)) {
            m_confirm = Confirm::Quit;
        }
        ImGui::Dummy({0.0f, 4.0f * scale});
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void GameApp::drawConfirmation() {
    const float scale = m_displayScale * m_settings.uiScale;
    if (beginCentered("##confirm", 360.0f, scale)) {
        ImGui::TextWrapped("%s", m_confirm == Confirm::Quit ? "¿Salir al escritorio?"
                                                            : "¿Volver al menú principal?");
        ImGui::TextDisabled("Se perderá lo que no hayas guardado (F5).");
        ImGui::Dummy({0.0f, 6.0f * scale});
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(m_confirm == Confirm::Quit ? "Salir" : "Volver al menú", {half, 32.0f * scale})) {
            if (m_confirm == Confirm::Quit) {
                m_quit = true;
            } else {
                m_pending = PendingAction::MainMenu;
            }
            m_confirm = Confirm::None;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", {half, 32.0f * scale})) {
            m_confirm = Confirm::None;
        }
    }
    ImGui::End();
}

void GameApp::drawOptionsWindow() {
    const float scale = m_displayScale * m_settings.uiScale;
    if (beginCentered("##options", 460.0f, scale)) {
        ImGui::BeginDisabled(m_confirm != Confirm::None);
        ImGui::PushFont(nullptr, 24.0f);
        ImGui::TextUnformatted("Opciones");
        ImGui::PopFont();
        ImGui::Separator();
        bool changed = false;

        ImGui::SeparatorText("Pantalla");
        int mode = m_settings.fullscreen ? 1 : 0;
        changed |= ImGui::RadioButton("Ventana", &mode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Pantalla completa (F11)", &mode, 1);
        m_settings.fullscreen = mode == 1;
        changed |= ImGui::Checkbox("Sincronización vertical", &m_settings.vsync);

        ImGui::SeparatorText("Interfaz");
        // Applied on release: rescaling while dragging would move the slider under the mouse.
        static float pendingScale = 0.0f;
        if (!ImGui::IsAnyItemActive()) {
            pendingScale = m_settings.uiScale * 100.0f;
        }
        ImGui::SetNextItemWidth(220.0f * scale);
        ImGui::SliderFloat("Escala", &pendingScale, UserSettings::kMinUiScale * 100.0f,
                           UserSettings::kMaxUiScale * 100.0f, "%.0f %%");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_settings.uiScale =
                std::clamp(pendingScale / 100.0f, UserSettings::kMinUiScale, UserSettings::kMaxUiScale);
            changed = true;
        }
        changed |= ImGui::Checkbox("Mostrar los controles (F1) al empezar", &m_settings.showHelpOnStart);

        ImGui::SeparatorText("Datos");
        const std::string folder = pathToUtf8(m_dataDir);
        ImGui::TextDisabled("Partidas, registro y preferencias:");
        ImGui::TextWrapped("%s", folder.c_str());
        if (ImGui::SmallButton("Abrir la carpeta")) {
            const std::string url = "file:///" + folder;
            if (!SDL_OpenURL(url.c_str())) {
                setStatus(std::string("No se pudo abrir la carpeta: ") + SDL_GetError());
            }
        }

        ImGui::Dummy({0.0f, 6.0f * scale});
        if (ImGui::Button("Volver (Esc)", {-FLT_MIN, 32.0f * scale})) {
            m_showOptions = false;
        }
        ImGui::EndDisabled();
        if (changed) {
            applySettings();
            saveSettings();
        }
    }
    ImGui::End();
}

} // namespace gx
