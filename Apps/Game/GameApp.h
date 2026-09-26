#pragma once

#include "Apps/Game/InspectorView.h"
#include "Apps/Game/MapView.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Time/TimeController.h"
#include "Game/Presentation/SystemSnapshot.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Kernel/Simulation.h"

#include <memory>
#include <string>

struct SDL_Window;
struct SDL_Renderer;

namespace gx {

// Graphical client for the Sandbox vertical slice. The client owns presentation only: every change to the
// simulation goes through PilotCommand, and it reads the simulation through SystemSnapshot.
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
        bool select = false;    // select the player ship (shows the inspector)
        bool showTruth = false; // debug omniscient view
        i32 flyToPort = -1;     // order the player ship to this port (index in Sandbox::ports())
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

    bool initPlatform();
    void shutdownPlatform();

    void newGame();
    void quickSave();
    void quickLoad();
    void resetTimeController();
    void advanceSimulation(u64 realDeltaNs);

    void handleKeyboard();
    void handleMap(const MapView::Interaction& interaction);
    void submitPilot(FlightMode mode, EntityId target = {}, const Vec3d& point = {},
                     const Vec3d& thrust = {});
    void submitSensors(bool activeOn, bool transponderOn);

    void drawTimeBar();
    void drawShipPanel();
    void drawSelectionPanel();
    void drawContactSelection(const ContactView& contact);
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
    u32 m_selectedContact = 0; // sensor track selected on the map (exclusive with m_selected)
    bool m_showTruth = false;  // debug: omniscient view

    Vec3d m_sentThrust; // last manual thrust sent, to only send changes
    bool m_thrusting = false;

    bool m_showHelp = true;
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
