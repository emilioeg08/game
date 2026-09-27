#include "Apps/Game/GameApp.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/SaveFile.h"
#include "Engine/Time/Stopwatch.h"
#include "Game/Sandbox/Content.h"
#include "Space/Bodies/CelestialBody.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>

namespace gx {
namespace {

constexpr std::string_view kChannel = "Game";
// x1 is real time (ADR-021): the game is played in real time with modest acceleration.
constexpr std::array<u32, 3> kSpeeds = {1, 3, 10};
constexpr std::array<const char*, 3> kSpeedLabels = {"x1", "x3", "x10"};
constexpr u64 kSimulationBudgetNs = 8'000'000; // per frame: keeps the client responsive at any speed
constexpr u64 kStatusDurationNs = 4'000'000'000;
constexpr const char* kSaveDirectory = "saves";
constexpr const char* kQuickSavePath = "saves/quicksave.gxsave";
constexpr f64 kStandardGravity = 9.80665;

// Deferred actions: executed at the start of a frame so no view of the old session survives mid-frame.
enum class PendingAction { None, NewGame, Save, Load };
PendingAction g_pendingAction = PendingAction::None;

ImVec4 color(u8 r, u8 g, u8 b, u8 a = 255) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
            static_cast<float>(a) / 255.0f};
}

void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = color(14, 18, 28, 225);
    style.Colors[ImGuiCol_TitleBg] = color(20, 28, 44, 255);
    style.Colors[ImGuiCol_TitleBgActive] = color(28, 44, 70, 255);
    style.Colors[ImGuiCol_Border] = color(60, 80, 110, 160);
    style.Colors[ImGuiCol_Button] = color(34, 52, 80, 255);
    style.Colors[ImGuiCol_ButtonHovered] = color(50, 80, 120, 255);
    style.Colors[ImGuiCol_ButtonActive] = color(70, 110, 160, 255);
    style.Colors[ImGuiCol_Header] = color(34, 52, 80, 255);
    style.Colors[ImGuiCol_HeaderHovered] = color(50, 80, 120, 255);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

} // namespace

GameApp::Session::Session(JobSystem& jobs, const SandboxConfig& config)
    : sandbox(config), simulation(Simulation::Config{.seed = config.seed}, jobs) {
    sandbox.install(simulation);
}

GameApp::GameApp() : m_jobs(JobSystem::defaultWorkerCount()) {}

GameApp::~GameApp() = default;

bool GameApp::initPlatform() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        GX_LOG_ERROR(kChannel, "SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    m_window = SDL_CreateWindow("GalaxyEngine - Sandbox", static_cast<int>(1440 * scale),
                                static_cast<int>(900 * scale),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (m_window == nullptr) {
        GX_LOG_ERROR(kChannel, "SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (m_renderer == nullptr) {
        GX_LOG_ERROR(kChannel, "SDL_CreateRenderer failed: {}", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(m_renderer, 1);
    SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(m_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No keyboard navigation: the keyboard belongs to the game (WASD, speeds) unless a text field is active.
    const std::filesystem::path uiFont = "C:/Windows/Fonts/segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::exists(uiFont, ec)) {
        io.Fonts->AddFontFromFileTTF(uiFont.string().c_str(), 17.0f);
    }
    applyStyle(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer3_Init(m_renderer);
    GX_LOG_INFO(kChannel, "window ready ({}), renderer '{}', UI scale {:.2f}", SDL_GetCurrentVideoDriver(),
                SDL_GetRendererName(m_renderer), scale);
    return true;
}

void GameApp::shutdownPlatform() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_renderer != nullptr) {
        SDL_DestroyRenderer(m_renderer);
    }
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
    }
    SDL_Quit();
}

int GameApp::run(const Options& options) {
    platform::setCurrentThreadName("main");
    if (!initPlatform()) {
        shutdownPlatform();
        return 1;
    }
    m_config.seed = options.seed;
    newGame();
    if (options.flyToPort >= 0 && static_cast<usize>(options.flyToPort) < sandbox().ports().size()) {
        submitPilot(FlightMode::Approach, sandbox().ports()[static_cast<usize>(options.flyToPort)]);
    }
    if (options.prerunHours > 0.0) {
        simulation().runFor(SimDuration::microseconds(static_cast<i64>(options.prerunHours * 3.6e9)));
        resetTimeController();
    }
    if (options.engageNearest) {
        // Capture helper: what pressing E on the nearest contact does, then a few seconds of the fight.
        m_snapshotBuilder.build(simulation(), sandbox(), m_snapshot);
        Vec3d player;
        const ContactView* nearest = nullptr;
        if (m_snapshot.positionOf(sandbox().playerShip(), player)) {
            for (const ContactView& contact : m_snapshot.contacts) {
                if (nearest == nullptr ||
                    lengthSquared(contact.position - player) < lengthSquared(nearest->position - player)) {
                    nearest = &contact;
                }
            }
        }
        if (nearest != nullptr) {
            m_selectedContact = nearest->trackId;
            submitEngage(nearest->trackId, true, true);
            simulation().runFor(SimDuration::seconds(3));
            resetTimeController();
        }
    }
    if (options.metersPerPixel > 0.0) {
        m_map.camera().metersPerPixel = options.metersPerPixel;
    }
    if (options.followStar && !sandbox().ports().empty()) {
        m_map.camera().follow = simulation().world().components<CelestialBody>().entities()[0];
    }
    if (options.select) {
        m_selected = sandbox().playerShip();
    }
    if (options.selectPort >= 0 && static_cast<usize>(options.selectPort) < sandbox().ports().size()) {
        m_selected = sandbox().ports()[static_cast<usize>(options.selectPort)];
    }
    m_showTruth = options.showTruth;

    u32 frame = 0;
    u64 lastNs = platform::monotonicNanoseconds();
    while (!m_quit) {
        ++frame;
        const bool lastFrame = options.frames != 0 && frame >= options.frames;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                 event.window.windowID == SDL_GetWindowID(m_window))) {
                m_quit = true;
            }
        }
        const u64 nowNs = platform::monotonicNanoseconds();
        const u64 realDeltaNs = std::min<u64>(nowNs - lastNs, 250'000'000); // a stall is not "time passing"
        lastNs = nowNs;
        if ((SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_Delay(10);
            continue;
        }

        switch (std::exchange(g_pendingAction, PendingAction::None)) {
        case PendingAction::NewGame:
            m_config.seed = mix64(nowNs) % 1'000'000; // fresh galaxy, still a readable seed
            newGame();
            break;
        case PendingAction::Save:
            quickSave();
            break;
        case PendingAction::Load:
            quickLoad();
            break;
        case PendingAction::None:
            break;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        handleKeyboard();
        advanceSimulation(realDeltaNs);
        m_snapshotBuilder.build(simulation(), sandbox(), m_snapshot);
        if (sandbox().playerShip() != m_knownPlayer) {
            // A replacement ship after the old one was destroyed: the camera goes with the player.
            if (sandbox().playerShip().isValid() &&
                (m_map.camera().follow == m_knownPlayer || !m_map.camera().follow.isValid())) {
                m_map.camera().follow = sandbox().playerShip();
            }
            m_knownPlayer = sandbox().playerShip();
        }
        handleMap(
            m_map.update(m_snapshot, MapSelection{m_selected, m_selectedContact}, MapOptions{m_showTruth}));

        drawTimeBar();
        drawShipPanel();
        drawSelectionPanel();
        drawSensorsPanel();
        drawMarketWindow();
        drawJournal();
        if (m_showDebug) {
            drawDebugPanel();
        }
        if (m_showHelp) {
            drawHelp();
        }
        if (!m_snapshot.playerAlive) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            const std::string text = std::format("NAVE DESTRUIDA  ·  una nave nueva te espera en {:.0f} s",
                                                 m_snapshot.playerRespawnIn);
            ImGui::GetForegroundDrawList()->AddText({display.x * 0.5f - 170.0f, display.y * 0.5f - 60.0f},
                                                    IM_COL32(255, 90, 80, 255), text.c_str());
        }
        if (!m_status.empty() && nowNs < m_statusUntilNs) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::GetForegroundDrawList()->AddText({display.x * 0.5f - 150.0f, display.y - 40.0f},
                                                    IM_COL32(255, 214, 90, 255), m_status.c_str());
        }

        ImGui::Render();
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(m_renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(m_renderer, 7, 9, 15, 255);
        SDL_RenderClear(m_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_renderer);
        if (lastFrame) {
            m_quit = true;
            if (!options.screenshot.empty()) {
                SDL_SetRenderScale(m_renderer, 1.0f, 1.0f);
                if (SDL_Surface* capture = SDL_RenderReadPixels(m_renderer, nullptr)) {
                    const bool png = options.screenshot.ends_with(".png");
                    if (png ? SDL_SavePNG(capture, options.screenshot.c_str())
                            : SDL_SaveBMP(capture, options.screenshot.c_str())) {
                        GX_LOG_INFO(kChannel, "screenshot saved to {}", options.screenshot);
                    } else {
                        GX_LOG_ERROR(kChannel, "cannot save screenshot: {}", SDL_GetError());
                    }
                    SDL_DestroySurface(capture);
                }
            }
        }
        SDL_RenderPresent(m_renderer);
    }
    shutdownPlatform();
    return 0;
}

// --- Session
// -------------------------------------------------------------------------------------------------

void GameApp::newGame() {
    m_session = std::make_unique<Session>(m_jobs, m_config);
    sandbox().populate(simulation());
    m_snapshotBuilder = SnapshotBuilder{};
    m_selected = {};
    m_selectedContact = 0;
    m_sentThrust = {};
    m_thrusting = false;
    m_knownPlayer = sandbox().playerShip();
    m_map.camera().follow = sandbox().playerShip();
    m_map.camera().metersPerPixel = 30'000.0;
    resetTimeController();
    setStatus(std::format("Nueva partida: sistema {} (semilla {})", sandbox().systemName(), m_config.seed));
}

void GameApp::quickSave() {
    std::error_code ec;
    std::filesystem::create_directories(kSaveDirectory, ec);
    const std::vector<std::byte> payload = simulation().saveState();
    const std::string description = std::format("Sandbox {} - {}", sandbox().systemName(),
                                                formatSimTime(simulation().now(), content::kEpochYear));
    std::string error;
    if (writeSaveFile(kQuickSavePath, GX_VERSION, description, payload, error)) {
        setStatus(std::format("Partida guardada en {} ({:.1f} KiB)", kQuickSavePath,
                              static_cast<f64>(payload.size()) / 1024.0));
    } else {
        setStatus("Error al guardar: " + error);
    }
}

void GameApp::quickLoad() {
    SaveFileContents contents;
    std::string error;
    if (!readSaveFile(kQuickSavePath, contents, error)) {
        setStatus("No se pudo cargar: " + error);
        return;
    }
    // Load into a fresh session and swap only on success: a failed load leaves the current game untouched.
    auto loaded = std::make_unique<Session>(m_jobs, m_config);
    if (!loaded->simulation.loadState(contents.payload, error)) {
        setStatus("Partida incompatible: " + error);
        return;
    }
    m_session = std::move(loaded);
    m_snapshotBuilder = SnapshotBuilder{};
    m_selected = {};
    m_selectedContact = 0;
    m_sentThrust = {};
    m_thrusting = false;
    m_knownPlayer = sandbox().playerShip();
    m_map.camera().follow = sandbox().playerShip();
    resetTimeController();
    setStatus(std::format("Partida cargada: {}", contents.info.description));
}

void GameApp::resetTimeController() {
    m_time = TimeController(simulation().now());
    m_speedWindowStartNs = platform::monotonicNanoseconds();
    m_speedWindowStartSim = simulation().now();
    m_effectiveSpeed = -1.0; // not measured yet
}

void GameApp::advanceSimulation(u64 realDeltaNs) {
    m_time.setPaused(m_paused);
    m_time.setSpeed(kSpeeds[m_speedIndex]);
    const SimTime target = m_time.update(realDeltaNs, simulation().now());
    const u64 stepsBefore = simulation().stepCount();
    const Stopwatch timer;
    simulation().runUntil(target, kSimulationBudgetNs);
    m_stepsLastFrame = simulation().stepCount() - stepsBefore;
    m_simulationMsLastFrame = timer.elapsedMs();

    const u64 nowNs = platform::monotonicNanoseconds();
    if (nowNs - m_speedWindowStartNs >= 500'000'000) {
        m_effectiveSpeed = (simulation().now() - m_speedWindowStartSim).toSeconds() /
                           (static_cast<f64>(nowNs - m_speedWindowStartNs) / 1e9);
        m_speedWindowStartNs = nowNs;
        m_speedWindowStartSim = simulation().now();
    }
}

// --- Input -------------------------------------------------------------------------------------------------

void GameApp::submitPilot(FlightMode mode, EntityId target, const Vec3d& point, const Vec3d& thrust) {
    simulation().submitCommand(PilotCommand{sandbox().playerShip(), mode, target, point, thrust});
}

void GameApp::submitSensors(bool activeOn, bool transponderOn) {
    simulation().submitCommand(SensorCommand{sandbox().playerShip(), activeOn, transponderOn});
}

void GameApp::submitEngage(u32 track, bool fire, bool pursue) {
    simulation().submitCommand(EngageCommand{sandbox().playerShip(), track, fire, pursue});
}

void GameApp::submitTrade(GoodId good, i32 tonnes) {
    simulation().submitCommand(TradeCommand{sandbox().playerShip(), good, tonnes});
}

void GameApp::handleKeyboard() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        m_paused = !m_paused;
    }
    for (usize i = 0; i < kSpeeds.size(); ++i) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(i)), false)) {
            m_speedIndex = i;
            m_paused = false;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        m_showHelp = !m_showHelp;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
        m_showDebug = !m_showDebug;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        g_pendingAction = PendingAction::Save;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        g_pendingAction = PendingAction::Load;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_selected = {};
        m_selectedContact = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        submitSensors(!m_snapshot.playerSensors.activeOn, m_snapshot.playerSensors.transponderOn);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        submitSensors(m_snapshot.playerSensors.activeOn, !m_snapshot.playerSensors.transponderOn);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
        m_map.camera().follow = sandbox().playerShip();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && m_selected.isValid()) {
        m_map.camera().follow = m_selected;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) && m_selectedContact != 0) {
        submitEngage(m_selectedContact, true, true);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        submitEngage(0, false, false);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        submitPilot(FlightMode::Stop);
        m_thrusting = false;
        m_sentThrust = {};
    }

    // Manual piloting: thrust along the pressed directions (map up = +y). Only changes are sent.
    const auto axis = [](ImGuiKey positive, ImGuiKey negative) {
        return (ImGui::IsKeyDown(positive) ? 1.0 : 0.0) - (ImGui::IsKeyDown(negative) ? 1.0 : 0.0);
    };
    Vec3d thrust{axis(ImGuiKey_D, ImGuiKey_A), axis(ImGuiKey_W, ImGuiKey_S), 0.0};
    if (thrust != m_sentThrust) {
        m_sentThrust = thrust;
        const f64 magnitude = length(thrust);
        if (magnitude > 0.0) {
            submitPilot(FlightMode::Manual, {}, {}, thrust / magnitude);
            m_thrusting = true;
        } else if (m_thrusting) {
            submitPilot(FlightMode::Manual); // engines off: keep drifting (Newtonian); X brakes
        }
    }
}

void GameApp::handleMap(const MapView::Interaction& interaction) {
    if (interaction.leftClicked) {
        m_selected = interaction.leftClickedTarget.entity;
        m_selectedContact = interaction.leftClickedTarget.contact;
    }
    if (interaction.rightClicked) {
        const MapSelection& target = interaction.rightClickedTarget;
        if (target.entity.isValid() && target.entity != sandbox().playerShip()) {
            submitPilot(FlightMode::Approach, target.entity);
        } else if (const ContactView* contact = m_snapshot.findContact(target.contact)) {
            // A contact is only an estimate: fly to where the sensors place it, not to the real ship.
            submitPilot(FlightMode::MoveTo, {}, contact->position);
        } else {
            submitPilot(FlightMode::MoveTo, {}, interaction.rightClickedPoint);
        }
        m_thrusting = false;
    }
}

// --- UI ----------------------------------------------------------------------------------------------------

std::string GameApp::nameOf(EntityId entity) const {
    if (const ShipView* ship = m_snapshot.findShip(entity)) {
        return std::string(ship->name);
    }
    if (const BodyView* body = m_snapshot.findBody(entity)) {
        return std::string(body->name);
    }
    return entity.isValid() ? std::format("#{}", entity.index) : std::string("-");
}

void GameApp::setStatus(std::string message) {
    GX_LOG_INFO(kChannel, "{}", message);
    m_status = std::move(message);
    m_statusUntilNs = platform::monotonicNanoseconds() + kStatusDurationNs;
}

void GameApp::drawTimeBar() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, 8.0f}, ImGuiCond_Always, {0.5f, 0.0f});
    ImGui::Begin("##time", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    const std::string date = formatSimTime(simulation().now(), content::kEpochYear);
    ImGui::Text("Sistema %s   |   %s", sandbox().systemName().c_str(), date.c_str());
    if (ImGui::Button(m_paused ? "Reanudar" : "  Pausa  ")) {
        m_paused = !m_paused;
    }
    for (usize i = 0; i < kSpeeds.size(); ++i) {
        ImGui::SameLine();
        const bool active = !m_paused && i == m_speedIndex;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, color(60, 110, 70));
        }
        if (ImGui::Button(kSpeedLabels[i])) {
            m_speedIndex = i;
            m_paused = false;
        }
        if (active) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::SameLine();
    const f64 requested = m_paused ? 0.0 : static_cast<f64>(kSpeeds[m_speedIndex]);
    if (m_effectiveSpeed < 0.0) {
        ImGui::TextDisabled("real —"); // not measured yet
    } else if (!m_paused && m_effectiveSpeed < requested * 0.9) {
        ImGui::TextColored(color(255, 170, 80), "real x%.0f (limitado por CPU)", m_effectiveSpeed);
    } else {
        ImGui::TextDisabled("real x%.0f", m_effectiveSpeed);
    }
    ImGui::End();
}

void GameApp::drawShipPanel() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({10.0f, 90.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({330.0f, display.y * 0.55f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Nave");
    const ShipView* ship = m_snapshot.findShip(sandbox().playerShip());
    if (ship == nullptr) {
        ImGui::TextColored(color(255, 110, 100), "Tu nave ha sido destruida.");
        ImGui::TextDisabled("Una nave nueva te espera en %s dentro de %.0f s.",
                            nameOf(sandbox().homePort()).c_str(), m_snapshot.playerRespawnIn);
        ImGui::End();
        return;
    }
    ImGui::Text("%.*s", static_cast<int>(ship->name.size()), ship->name.data());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", content::kShipClasses[ship->shipClass].name);
    u32 cargoUsed = 0;
    std::string cargo;
    for (const CargoItem& item : m_snapshot.playerCargo) {
        cargoUsed += item.tonnes;
        cargo += std::format("{}{} t {}", cargo.empty() ? "" : ", ", item.tonnes,
                             sandbox().economy().goods()[item.good].name);
    }
    ImGui::Text("Créditos: %lld cr   ·   Bodega: %u / %u t", static_cast<long long>(m_snapshot.playerCredits),
                cargoUsed, m_snapshot.playerCargoCapacity);
    if (!cargo.empty()) {
        ImGui::TextDisabled("Carga: %s", cargo.c_str());
    }
    ImGui::Separator();
    const f64 acceleration = length(ship->acceleration);
    ImGui::Text("Velocidad:    %s", formatSpeed(length(ship->velocity)).c_str());
    ImGui::Text("Aceleración:  %.0f m/s² (%.0f g)", acceleration, acceleration / kStandardGravity);
    ImGui::Text("Modo:         %s", displayName(ship->mode));
    if (ship->phase == DrivePhase::Charging) {
        ImGui::TextColored(color(200, 160, 255), "Motor:        cargando salto (%.1f s)",
                           ship->chargeRemaining);
    } else {
        ImGui::Text("Motor:        %s", displayName(ship->phase));
    }
    if (ship->mode == FlightMode::Approach || ship->mode == FlightMode::MoveTo) {
        const std::string target =
            ship->mode == FlightMode::Approach ? nameOf(ship->target) : "punto del espacio";
        ImGui::Text("Destino:      %s", target.c_str());
        ImGui::Text("Distancia:    %s",
                    formatDistance(length(ship->targetPosition - ship->position)).c_str());
        ImGui::Text("Estado:       %s", ship->arrived ? "en posición" : "en ruta");
    } else if (ship->mode == FlightMode::Pursue) {
        const ContactView* chased = m_snapshot.findContact(ship->track);
        ImGui::Text("Persigue:     %s",
                    chased != nullptr ? contactLabel(*chased).c_str() : "contacto perdido");
        if (chased != nullptr) {
            ImGui::Text("Distancia:    %s",
                        formatDistance(length(chased->position - ship->position)).c_str());
        }
    }
    if (ImGui::Button("Detener (X)")) {
        submitPilot(FlightMode::Stop);
    }
    ImGui::SameLine();
    if (ImGui::Button("Deriva")) {
        submitPilot(FlightMode::Coast);
    }
    ImGui::SameLine();
    if (ImGui::Button("Seguir (H)")) {
        m_map.camera().follow = ship->id;
    }
    drawCombatSection(*ship);

    ImGui::Separator();
    ImGui::TextDisabled("Destinos (clic para fijar rumbo)");
    ImGui::BeginChild("ports", {0.0f, 0.0f}, ImGuiChildFlags_None);
    for (const EntityId port : sandbox().ports()) {
        const BodyView* body = m_snapshot.findBody(port);
        if (body == nullptr) {
            continue;
        }
        const std::string label =
            std::format("{}  ·  {}  ·  {}###port{}", body->name, displayName(body->kind),
                        formatDistance(length(body->position - ship->position)), port.index);
        if (ImGui::Selectable(label.c_str(), ship->target == port)) {
            submitPilot(FlightMode::Approach, port);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void GameApp::drawCombatSection(const ShipView& ship) {
    ImGui::Separator();
    if (m_snapshot.tactical) {
        ImGui::TextColored(color(255, 170, 80), "Modo táctico");
        ImGui::SameLine();
    }
    if (!ship.powered) {
        ImGui::TextColored(color(255, 90, 80), "SIN ENERGÍA: a la deriva hasta que se repare el reactor");
    } else if (ship.fireTrack != 0) {
        const ContactView* target = m_snapshot.findContact(ship.fireTrack);
        ImGui::TextColored(color(255, 110, 100), "Fuego sobre: %s",
                           target != nullptr ? contactLabel(*target).c_str() : "contacto perdido");
        if (ImGui::SmallButton("Alto el fuego (C)")) {
            submitEngage(0, false, false);
        }
    } else {
        ImGui::TextDisabled("Armas en espera");
    }
    const std::vector<WeaponDef>& weapons = sandbox().combat().weapons();
    for (const ModuleView& module : m_snapshot.playerModules) {
        const ImVec4 barColor = !module.functional      ? color(200, 60, 50)
                                : module.fraction < 0.6 ? color(220, 170, 60)
                                                        : color(80, 170, 100);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
        const std::string percent = std::format("{:.0f}%", module.fraction * 100.0);
        ImGui::ProgressBar(static_cast<float>(module.fraction), {70.0f, 0.0f}, percent.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const bool isWeapon = module.type == ModuleType::Weapon && module.weapon < weapons.size();
        const char* name = isWeapon ? weapons[module.weapon].name.c_str() : displayName(module.type);
        if (!module.functional) {
            ImGui::TextColored(color(255, 110, 100), "%s (fuera de servicio)", name);
            continue;
        }
        if (!isWeapon) {
            ImGui::TextUnformatted(name);
            continue;
        }
        const FireSolution& fire = module.fire;
        if (!fire.hasTarget) {
            ImGui::Text("%s", name);
        } else if (!fire.inRange) {
            ImGui::Text("%s  ·  fuera de alcance (%s)", name,
                        formatDistance(weapons[module.weapon].range).c_str());
        } else if (!fire.locked) {
            ImGui::TextColored(color(220, 170, 60), "%s  ·  sin fijación", name);
        } else if (module.cooldown > 0.0) {
            ImGui::TextColored(color(120, 255, 220), "%s  ·  recargando %.1f s", name, module.cooldown);
        } else {
            ImGui::TextColored(color(120, 255, 220), "%s  ·  fijado", name);
        }
    }
}

void GameApp::drawMarketWindow() {
    const MarketView* market = m_snapshot.findMarket(m_snapshot.dockedPort);
    if (market == nullptr) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({460.0f, display.y - 20.0f}, ImGuiCond_FirstUseEver, {0.0f, 1.0f});
    ImGui::SetNextWindowSize({620.0f, 250.0f}, ImGuiCond_FirstUseEver);
    const std::string title = std::format("Mercado · {}###market", nameOf(market->port));
    ImGui::Begin(title.c_str());
    ImGui::Text("Créditos: %lld cr", static_cast<long long>(m_snapshot.playerCredits));
    ImGui::SameLine();
    u32 used = 0;
    for (const CargoItem& item : m_snapshot.playerCargo) {
        used += item.tonnes;
    }
    ImGui::TextDisabled("  ·  bodega %u / %u t  ·  compras a la izquierda, ventas a la derecha", used,
                        m_snapshot.playerCargoCapacity);
    if (ImGui::BeginTable("market", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Bien");
        ImGui::TableSetupColumn("Existencias");
        ImGui::TableSetupColumn("Prod. / cons.");
        ImGui::TableSetupColumn("Compra");
        ImGui::TableSetupColumn("Venta");
        ImGui::TableSetupColumn("Bodega");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const MarketRowView& row : market->rows) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(row.good));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.name.data(), row.name.data() + row.name.size());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f t", row.stock, row.target);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("+%.0f / -%.0f t/h", row.production, row.consumption);
            // Prices against the good's base: cheap in green, dear in red.
            const auto priceColor = [&](i64 price) {
                const f64 ratio = static_cast<f64>(price) / row.basePrice;
                return ratio < 0.8    ? color(120, 230, 140)
                       : ratio > 1.25 ? color(255, 130, 110)
                                      : color(220, 220, 220);
            };
            ImGui::TableNextColumn();
            ImGui::TextColored(priceColor(row.buy), "%lld", static_cast<long long>(row.buy));
            ImGui::TableNextColumn();
            ImGui::TextColored(priceColor(row.sell), "%lld", static_cast<long long>(row.sell));
            ImGui::TableNextColumn();
            u32 held = 0;
            for (const CargoItem& item : m_snapshot.playerCargo) {
                held = item.good == row.good ? item.tonnes : held;
            }
            ImGui::Text("%u t", held);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("+1")) {
                submitTrade(row.good, 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+10")) {
                submitTrade(row.good, 10);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (ImGui::SmallButton("-1")) {
                submitTrade(row.good, -1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Todo")) {
                submitTrade(row.good, -static_cast<i32>(std::max<u32>(held, 1)));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void GameApp::drawKnownPrices(EntityId port) {
    const KnownPricesView* known = m_snapshot.findKnownPrices(port);
    if (known == nullptr || known->prices == nullptr) {
        ImGui::TextDisabled("Sin datos de mercado: nadie te ha contado sus precios.");
        return;
    }
    ImGui::TextDisabled("Precios que conoces (de hace %s)",
                        formatDuration(SimDuration::seconds(static_cast<i64>(known->ageSeconds))).c_str());
    if (ImGui::BeginTable("known", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Bien");
        ImGui::TableSetupColumn("Compra");
        ImGui::TableSetupColumn("Venta");
        ImGui::TableSetupColumn("Existencias");
        ImGui::TableHeadersRow();
        for (const PricePoint& point : known->prices->prices) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sandbox().economy().goods()[point.good].name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(point.buy));
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(point.sell));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(point.stockRatio < 0.3   ? "escasas"
                                   : point.stockRatio < 0.8 ? "bajas"
                                   : point.stockRatio < 1.5 ? "normales"
                                                            : "abundantes");
        }
        ImGui::EndTable();
    }
}

void GameApp::drawEconomyInspector() {
    const SandboxStats& stats = sandbox().stats();
    ImGui::Text(
        "Comerciantes: %llu cargas, %llu t entregadas, %llu viajes de reposición, %llu de exploración",
        static_cast<unsigned long long>(stats.haulerTrades),
        static_cast<unsigned long long>(stats.tonnesDelivered),
        static_cast<unsigned long long>(stats.repositionTrips),
        static_cast<unsigned long long>(stats.explorationTrips));
    for (const MarketView& market : m_snapshot.markets) {
        const std::string label =
            std::format("{} (peligro {:.2f})###eco{}", nameOf(market.port),
                        sandbox().danger(market.port, simulation().now()), market.port.index);
        if (!ImGui::TreeNode(label.c_str())) {
            continue;
        }
        if (ImGui::BeginTable("eco", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Bien");
            ImGui::TableSetupColumn("Existencias");
            ImGui::TableSetupColumn("Precio");
            ImGui::TableSetupColumn("Prod./cons. t/h");
            ImGui::TableSetupColumn("Escasez");
            ImGui::TableHeadersRow();
            for (const MarketRowView& row : market.rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.name.data(), row.name.data() + row.name.size());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f / %.0f", row.stock, row.target);
                ImGui::TableNextColumn();
                ImGui::Text("%lld", static_cast<long long>(row.buy));
                ImGui::TableNextColumn();
                ImGui::Text("+%.0f / -%.0f", row.production, row.consumption);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f t", row.shortage);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

void GameApp::drawContactSelection(const ContactView& contact) {
    if (contact.level == ContactLevel::Identified && !contact.name.empty()) {
        ImGui::Text("%.*s", static_cast<int>(contact.name.size()), contact.name.data());
    } else if (contact.level == ContactLevel::Classified) {
        ImGui::Text("Contacto %u: %s?", contact.trackId, content::kShipClasses[contact.shipClass].name);
    } else {
        ImGui::Text("Contacto desconocido %u", contact.trackId);
    }
    ImGui::TextDisabled("Sensores: %s", displayName(contact.level));
    if (contact.level == ContactLevel::Identified) {
        ImGui::TextDisabled("Facción: %s  ·  %s", content::kFactionNames[contact.faction],
                            content::kShipClasses[contact.shipClass].name);
    }
    Vec3d playerPosition;
    if (m_snapshot.positionOf(sandbox().playerShip(), playerPosition)) {
        ImGui::Text("Distancia estimada: %s",
                    formatDistance(length(contact.position - playerPosition)).c_str());
    }
    ImGui::Text("Incertidumbre: ±%s", formatDistance(contact.uncertainty).c_str());
    ImGui::Text("Velocidad estimada: %s", formatSpeed(length(contact.velocity)).c_str());
    ImGui::Text("Última detección: hace %.0f s", contact.ageSeconds);
    if (ImGui::Button("Ir a su posición estimada")) {
        submitPilot(FlightMode::MoveTo, {}, contact.position);
    }
    if (ImGui::Button("Interceptar")) {
        submitEngage(contact.trackId, false, true);
    }
    ImGui::SameLine();
    const ShipView* player = m_snapshot.findShip(sandbox().playerShip());
    if (player != nullptr && player->fireTrack == contact.trackId) {
        if (ImGui::Button("Alto el fuego (C)")) {
            submitEngage(0, false, false);
        }
    } else if (ImGui::Button("Atacar (E)")) {
        submitEngage(contact.trackId, true, true);
    }
    if (m_showTruth && contact.ghost) {
        ImGui::TextColored(color(255, 90, 200), "[depuración] contacto fantasma: no existe");
    }
}

void GameApp::drawSelectionPanel() {
    if (m_selectedContact != 0) {
        const ContactView* contact = m_snapshot.findContact(m_selectedContact);
        if (contact == nullptr) {
            m_selectedContact = 0; // contact lost
            return;
        }
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({display.x - 370.0f, 90.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({360.0f, display.y * 0.6f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Selección");
        drawContactSelection(*contact);
        ImGui::End();
        return;
    }
    if (!m_selected.isValid()) {
        return;
    }
    if (!simulation().world().isAlive(m_selected)) {
        m_selected = {};
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 370.0f, 90.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({360.0f, display.y * 0.6f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Selección");
    ImGui::Text("%s", nameOf(m_selected).c_str());
    const bool isPort = m_snapshot.findMarket(m_selected) != nullptr;
    if (const BodyView* body = m_snapshot.findBody(m_selected)) {
        ImGui::TextDisabled("%s  ·  radio %s", displayName(body->kind), formatDistance(body->radius).c_str());
    } else if (const ShipView* ship = m_snapshot.findShip(m_selected)) {
        ImGui::TextDisabled("%s  ·  %s  ·  %s", content::kShipClasses[ship->shipClass].name,
                            content::kFactionNames[ship->faction], displayName(ship->mode));
    }
    Vec3d selectedPosition;
    Vec3d playerPosition;
    if (m_snapshot.positionOf(m_selected, selectedPosition) &&
        m_snapshot.positionOf(sandbox().playerShip(), playerPosition)) {
        ImGui::TextDisabled("Distancia a tu nave: %s",
                            formatDistance(length(selectedPosition - playerPosition)).c_str());
    }
    if (m_selected != sandbox().playerShip() && ImGui::Button("Fijar rumbo")) {
        submitPilot(FlightMode::Approach, m_selected);
    }
    ImGui::SameLine();
    if (ImGui::Button("Seguir (F)")) {
        m_map.camera().follow = m_selected;
    }
    ImGui::Separator();
    if (isPort && ImGui::CollapsingHeader("Mercado", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_selected == m_snapshot.dockedPort) {
            ImGui::TextColored(color(120, 255, 150), "Atracado aquí: comercia en la ventana Mercado.");
        } else {
            drawKnownPrices(m_selected);
        }
    }
    if (ImGui::CollapsingHeader("Inspector de entidad", ImGuiTreeNodeFlags_DefaultOpen)) {
        const EntityId clicked = m_inspector.draw(simulation().world(), m_selected);
        if (clicked.isValid()) {
            m_selected = clicked;
        }
    }
    ImGui::End();
}

void GameApp::drawSensorsPanel() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 370.0f, display.y * 0.6f + 100.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({360.0f, display.y * 0.4f - 140.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Sensores");
    const SensorSuite& sensors = m_snapshot.playerSensors;
    const bool radarAvailable = sensors.activeStrength > 0.0;
    if (radarAvailable && ImGui::Button(sensors.activeOn ? "Apagar radar (R)" : "Encender radar (R)")) {
        submitSensors(!sensors.activeOn, sensors.transponderOn);
    }
    ImGui::SameLine();
    if (ImGui::Button(sensors.transponderOn ? "Apagar transpondedor (T)" : "Encender transpondedor (T)")) {
        submitSensors(sensors.activeOn, !sensors.transponderOn);
    }
    // How visible the player is, against the passive sensors of a typical hauler.
    const f64 haulerSensitivity = content::kShipSensors[content::kShipClassHauler].passiveSensitivity;
    ImGui::Text("Tu firma: %.2g", m_snapshot.playerEmission);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped(
        "Un carguero te detecta a %s%s",
        formatDistance(passiveDetectionRange(m_snapshot.playerEmission, haulerSensitivity)).c_str(),
        sensors.transponderOn ? " (y te identifica a 1 UA por el transpondedor)" : "");
    ImGui::PopStyleColor();
    ImGui::Separator();

    Vec3d playerPosition;
    if (!m_snapshot.positionOf(sandbox().playerShip(), playerPosition)) {
        ImGui::TextDisabled("Sin nave, no hay sensores.");
        ImGui::End();
        return;
    }
    std::vector<const ContactView*> contacts;
    for (const ContactView& contact : m_snapshot.contacts) {
        contacts.push_back(&contact);
    }
    std::sort(contacts.begin(), contacts.end(), [&](const ContactView* a, const ContactView* b) {
        return lengthSquared(a->position - playerPosition) < lengthSquared(b->position - playerPosition);
    });
    ImGui::TextDisabled("%zu contactos (clic para seleccionar)", contacts.size());
    if (ImGui::BeginTable("contacts", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Contacto");
        ImGui::TableSetupColumn("Nivel");
        ImGui::TableSetupColumn("Distancia");
        ImGui::TableSetupColumn("Hace");
        ImGui::TableHeadersRow();
        for (const ContactView* contact : contacts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string label = contactLabel(*contact) + std::format("###contact{}", contact->trackId);
            if (ImGui::Selectable(label.c_str(), m_selectedContact == contact->trackId,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedContact = contact->trackId;
                m_selected = {};
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(displayName(contact->level));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatDistance(length(contact->position - playerPosition)).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f s", contact->ageSeconds);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void GameApp::drawJournal() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({10.0f, display.y * 0.55f + 100.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440.0f, display.y * 0.45f - 140.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Diario");
    const auto& journal = sandbox().journal();
    for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
        ImGui::TextDisabled("%s", formatSimTime(it->time, content::kEpochYear).c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", it->text.c_str());
    }
    ImGui::End();
}

void GameApp::drawDebugPanel() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 480.0f, display.y * 0.6f + 100.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({470.0f, display.y * 0.4f - 140.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Depuración (F3)", &m_showDebug);
    const ImGuiIO& io = ImGui::GetIO();
    const SimulationStats& stats = simulation().stats();
    const SystemState& flight = simulation().scheduler().system(sandbox().flightSystem());
    ImGui::Text("%.0f FPS  (%.2f ms/frame)", io.Framerate, 1000.0f / io.Framerate);
    ImGui::Text("Simulación: %llu pasos en el último frame, %.2f ms",
                static_cast<unsigned long long>(m_stepsLastFrame), m_simulationMsLastFrame);
    ImGui::Text("Pasos totales: %llu  ·  eventos: %llu  ·  comandos: %llu",
                static_cast<unsigned long long>(stats.steps),
                static_cast<unsigned long long>(stats.eventsEmitted),
                static_cast<unsigned long long>(stats.commandsApplied));
    ImGui::Text("Vuelo y combate (LOD): paso de %s", formatDuration(flight.desc.period).c_str());
    const CombatStats& combat = sandbox().combat().stats();
    const SandboxStats& game = sandbox().stats();
    ImGui::Text("Combate: %llu disparos, %llu impactos, %llu naves destruidas",
                static_cast<unsigned long long>(combat.shotsFired),
                static_cast<unsigned long long>(combat.hits),
                static_cast<unsigned long long>(combat.shipsDestroyed));
    ImGui::Text(
        "Piratas: %llu cacerías, %llu abatidos, %llu huidos  ·  cargueros perdidos: %llu",
        static_cast<unsigned long long>(game.hunts), static_cast<unsigned long long>(game.piratesLost),
        static_cast<unsigned long long>(game.piratesLeft), static_cast<unsigned long long>(game.haulersLost));
    ImGui::Text("Entidades: %u  ·  semilla %llu  ·  rechazados: %llu", simulation().world().entityCount(),
                static_cast<unsigned long long>(m_config.seed),
                static_cast<unsigned long long>(sandbox().stats().commandsRejected));
    ImGui::Checkbox("Mostrar la verdad (omnisciencia de depuración)", &m_showTruth);
    if (ImGui::Button("Guardar (F5)")) {
        g_pendingAction = PendingAction::Save;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cargar (F9)")) {
        g_pendingAction = PendingAction::Load;
    }
    ImGui::SameLine();
    if (ImGui::Button("Nueva partida")) {
        g_pendingAction = PendingAction::NewGame;
    }
    if (ImGui::CollapsingHeader("Economía (verdad)")) {
        drawEconomyInspector();
    }
    if (ImGui::CollapsingHeader("Perfilador", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Reiniciar estadísticas")) {
            profiling::resetStats();
        }
        const std::vector<profiling::ZoneSummary> zones = profiling::summary();
        if (ImGui::BeginTable("zones", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Zona");
            ImGui::TableSetupColumn("Llamadas");
            ImGui::TableSetupColumn("Media µs");
            ImGui::TableSetupColumn("Máx µs");
            ImGui::TableHeadersRow();
            for (usize i = 0; i < std::min<usize>(zones.size(), 14); ++i) {
                const profiling::ZoneSummary& zone = zones[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(zone.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(zone.count));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", zone.meanNs() / 1e3);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", static_cast<f64>(zone.maxNs) / 1e3);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void GameApp::drawHelp() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, 90.0f}, ImGuiCond_FirstUseEver, {0.5f, 0.0f});
    ImGui::Begin("Controles (F1)", &m_showHelp, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BulletText("Rueda: zoom (de metros a unidades astronómicas)   ·   Arrastrar: mover la vista");
    ImGui::BulletText("Clic: seleccionar   ·   Clic derecho: ir a ese objeto o a ese punto");
    ImGui::BulletText("W A S D: empuje manual (newtoniano: la nave sigue derivando)   ·   X: frenar");
    ImGui::BulletText("Espacio: pausa   ·   1 / 2 / 3: velocidad x1 (tiempo real) / x3 / x10");
    ImGui::BulletText("Viajes largos: salto al hiperespacio fuera de los pozos gravitatorios (círculos)");
    ImGui::BulletText(
        "R: radar (ves más, pero te ven de lejos)   ·   T: transpondedor (difunde tu identidad)");
    ImGui::BulletText("E: atacar el contacto seleccionado (lo persigue y dispara)   ·   C: alto el fuego");
    ImGui::BulletText("Los piratas acechan junto a los pozos; cerca de las estaciones estás a salvo");
    ImGui::BulletText("Atracado en un puerto: compra y vende en la ventana Mercado. Las estaciones te dan el "
                      "boletín de precios de los comerciantes");
    ImGui::BulletText("H: seguir tu nave   ·   F: seguir la selección   ·   Esc: deseleccionar");
    ImGui::BulletText("F5: guardar   ·   F9: cargar   ·   F3: depuración   ·   F1: esta ayuda");
    ImGui::End();
}

} // namespace gx
