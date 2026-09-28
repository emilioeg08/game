#pragma once

#include "Apps/Game/InspectorView.h"
#include "Apps/Game/MapView.h"
#include "Engine/Core/Log.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Text/Localization.h"
#include "Engine/Time/TimeController.h"
#include "Game/Presentation/SystemSnapshot.h"
#include "Game/Presentation/UserSettings.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Kernel/Simulation.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

struct SDL_Window;
struct SDL_Renderer;

namespace gx {

// Graphical client for the Sandbox vertical slice. The client owns presentation only: every change to the
// simulation goes through commands (pilot, sensors, engage, trade), and it reads the simulation through
// SystemSnapshot.
class GameApp {
public:
    // Startup options, mostly for automated checks (render a few frames, capture, exit).
    struct Options {
        u64 seed = 2400;
        u32 frames = 0;         // exit after this many frames (0: run until closed)
        std::string screenshot; // PNG (or BMP) of the last frame (with frames > 0)
        f64 prerunHours = 0.0;  // simulate before the first frame
        f64 metersPerPixel = 0.0;
        bool followStar = false;
        bool select = false;        // select the player ship (shows the inspector)
        i32 selectPort = -1;        // select this port (index in Sandbox::ports()): its known prices
        bool showTruth = false;     // debug omniscient view
        i32 flyToPort = -1;         // order the player ship to this port (index in Sandbox::ports())
        bool engageNearest = false; // after the prerun: attack the nearest contact and fight for 3 s
        bool hideHelp = false;      // start with the controls window closed
        std::string dataDir;        // UTF-8; empty: the user's data folder (%APPDATA% on Windows)
        // Where to start: "" (the main menu; straight into the game when capturing frames), "main",
        // "pause" or "options" (captures of the menus).
        std::string menu;
    };

    GameApp();
    ~GameApp();
    GameApp(const GameApp&) = delete;
    GameApp& operator=(const GameApp&) = delete;

    int run(const Options& options);

private:
    // Everything that makes up one game: destroyed and rebuilt as a unit on "new game" and "load".
    struct Session {
        Session(JobSystem& jobs, const SandboxConfig& config);
        Sandbox sandbox; // declared first: the simulation's systems point into it
        Simulation simulation;
    };

    bool initPlatform(const Options& options);
    // Saves, logs and UI layout live in the user's profile, never next to the executable: a Steam install
    // folder may be read-only, and Steam Cloud synchronises a fixed per-user folder (ADR-034).
    bool initDataDirectory(const std::string& overridePath);
    void toggleFullscreen();
    void shutdownPlatform();

    void newGame();
    void quickSave();
    bool quickLoad();

    // Menus (Apps/Game/Menus.cpp, ADR-035). The main menu runs a fresh system in the background; the pause
    // menu stops the simulation and hides the HUD.
    enum class Screen : u8 { MainMenu, Playing };
    enum class Confirm : u8 { None, MainMenu, Quit };
    // Deferred actions: executed at the start of a frame so no view of the old session survives mid-frame.
    enum class PendingAction : u8 { None, NewGame, Save, Load, MainMenu };
    void enterMainMenu();
    void startPlaying();
    void handleMenuKeyboard();
    void drawMainMenu();
    void drawPauseMenu();
    void drawOptionsWindow();
    void drawConfirmation();
    void refreshSaveSummary();
    void applySettings();
    void saveSettings();
    // Languages (ADR-036): Spanish is built in; others are catalogs in <install>/data/lang/<code>.po.
    struct LanguageOption {
        std::string code;
        std::string name; // in its own language
    };
    void scanLanguages();
    void applyLanguage();
    [[nodiscard]] std::string resolveLanguage() const;
    [[nodiscard]] bool menuOpen() const;
    void resetTimeController();
    void advanceSimulation(u64 realDeltaNs);

    void handleKeyboard();
    void handleMap(const MapView::Interaction& interaction);
    void submitPilot(FlightMode mode, EntityId target = {}, const Vec3d& point = {},
                     const Vec3d& thrust = {});
    void submitSensors(bool activeOn, bool transponderOn);
    void submitEngage(u32 track, bool fire, bool pursue);
    void submitTrade(GoodId good, i32 tonnes);
    void submitBoard(u32 track);
    void submitContract(u32 contract, ContractAction action);

    void drawTimeBar();
    void drawShipPanel();
    void drawSelectionPanel();
    void drawContactSelection(const ContactView& contact);
    void drawCombatSection(const ShipView& ship);
    void drawMarketWindow();
    void drawContractsWindow();
    void drawKnownPrices(EntityId port);
    void drawEconomyInspector();
    void drawSensorsPanel();
    void drawJournal();
    void drawDebugPanel();
    void drawHelp();

    [[nodiscard]] Simulation& simulation() { return m_session->simulation; }
    [[nodiscard]] Sandbox& sandbox() { return m_session->sandbox; }
    [[nodiscard]] std::string nameOf(EntityId entity) const;
    void setStatus(std::string message);

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    std::filesystem::path m_dataDir;
    std::string m_imguiIniPath; // UTF-8, kept alive for ImGui
    std::shared_ptr<FileLogSink> m_logFile;
    UserSettings m_settings;
    Catalog m_catalog;
    std::vector<LanguageOption> m_languages;
    float m_displayScale = 1.0f; // the display's content scale; the UI uses it times m_settings.uiScale

    Screen m_screen = Screen::MainMenu;
    bool m_pauseMenu = false;
    bool m_showOptions = false;
    Confirm m_confirm = Confirm::None;
    PendingAction m_pending = PendingAction::None;
    bool m_seedPanel = false;
    u64 m_menuSeed = 2400;
    std::optional<u64> m_newGameSeed; // with PendingAction::NewGame; none: a random galaxy
    std::string m_saveSummary;        // the quick save's description, empty if there is none

    JobSystem m_jobs;
    SandboxConfig m_config;
    std::unique_ptr<Session> m_session;
    TimeController m_time{SimTime::epoch()};
    usize m_speedIndex = 0; // x1: real time
    bool m_paused = false;

    SnapshotBuilder m_snapshotBuilder;
    SystemSnapshot m_snapshot;
    MapView m_map;
    InspectorView m_inspector;
    EntityId m_selected;
    EntityId m_knownPlayer;    // to notice a replacement ship after the player's was destroyed
    u32 m_selectedContact = 0; // sensor track selected on the map (exclusive with m_selected)
    bool m_showTruth = false;  // debug: omniscient view

    Vec3d m_sentThrust; // last manual thrust sent, to only send changes
    bool m_thrusting = false;

    bool m_showHelp = true;
    bool m_showTraffic = false; // journal: NPC comings and goings
    bool m_showContracts = true;
    bool m_showDebug = false;
    bool m_quit = false;
    std::string m_status;
    u64 m_statusUntilNs = 0;

    // Measured, not requested, time acceleration.
    u64 m_speedWindowStartNs = 0;
    SimTime m_speedWindowStartSim;
    f64 m_effectiveSpeed = 0.0;
    u64 m_stepsLastFrame = 0;
    f64 m_simulationMsLastFrame = 0.0;
};

} // namespace gx
