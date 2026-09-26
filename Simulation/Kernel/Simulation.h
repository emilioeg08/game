#pragma once

#include "Engine/Memory/LinearArena.h"
#include "Engine/Time/SimClock.h"
#include "Simulation/Commands/CommandQueue.h"
#include "Simulation/Events/EventBus.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/World.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace gx {

class JobSystem;

struct SimulationStats {
    u64 steps = 0;
    u64 systemRuns = 0;
    u64 commandsApplied = 0;
    u64 eventsEmitted = 0;
};

// Headless simulation kernel. Owns the authoritative state: clock, system schedule, world (entities and
// components), pending commands, plus named state blocks registered by domain modules. Needs no renderer,
// UI or wall clock; interactive pacing lives outside (TimeController).
//
// One step: advance the clock to the next instant at which a system or a command is due, then
//   Commands phase        pending commands, then Commands systems
//   Simulation, Synchronization systems
//   EventResolution       event subscribers (cascades allowed), then EventResolution systems
//   History, PresentationSnapshot systems
//   end of step           deferred rate changes applied, events discarded, profiler collected
class Simulation {
public:
    struct Config {
        u64 seed = 0;
        SimTime startTime{};
        usize scratchBytes = 8u * 1024u * 1024u; // per-step scratch arena
    };

    Simulation(const Config& config, JobSystem& jobs);
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // --- Setup (not allowed during a step) -------------------------------------------------------------
    SystemId addSystem(SystemDesc desc);
    // Domain state that is not in the World (e.g. dense per-system arrays). Saved and loaded by name.
    void addStateBlock(std::string name, std::function<void(BinaryWriter&)> save,
                       std::function<void(BinaryReader&)> load);

    [[nodiscard]] World& world() { return m_world; }
    [[nodiscard]] const World& world() const { return m_world; }
    [[nodiscard]] EventBus& events() { return m_events; }
    [[nodiscard]] CommandQueue& commands() { return m_commands; }
    [[nodiscard]] JobSystem& jobs() { return m_jobs; }

    // --- Running -----------------------------------------------------------------------------------------
    // Executes the next step. Returns false if nothing is scheduled (no systems, no pending commands).
    bool step();

    // Runs every step scheduled at or before `target`, then moves the clock to `target`.
    // With a non-zero wall budget the call may return early (clock at the last executed step); calling again
    // resumes exactly where it stopped, so results never depend on how the run is sliced.
    u64 runUntil(SimTime target, u64 wallBudgetNs = 0);
    u64 runFor(SimDuration duration) { return runUntil(now() + duration); }

    // Queues an external command. It always executes strictly in the future (at least 1 us after now),
    // before any system of the step at that instant, identically when a recorded log is replayed.
    template <typename C>
    u64 submitCommand(const C& command, SimTime executeAt = SimTime::epoch()) {
        const SimTime earliest = now() + SimDuration::microseconds(1);
        return m_commands.submit(command, std::max(executeAt, earliest));
    }

    // Logical LOD switch. During a step the change is deferred to the end of the step.
    void setSystemPeriod(SystemId id, SimDuration period);
    [[nodiscard]] SystemId findSystem(std::string_view name) const { return m_scheduler.find(name); }

    // --- Persistence -------------------------------------------------------------------------------------
    // Serializes the complete simulation state (not allowed during a step).
    [[nodiscard]] std::vector<std::byte> saveState() const;
    // Restores state saved by a simulation with the same registrations (systems, components, events,
    // commands, state blocks). On failure returns false with a reason; the simulation must then be discarded.
    bool loadState(std::span<const std::byte> bytes, std::string& error);
    // Hash of saveState(): identical hashes mean identical simulations.
    [[nodiscard]] u64 stateHash() const;

    // --- Inspection --------------------------------------------------------------------------------------
    [[nodiscard]] SimTime now() const { return m_clock.now(); }
    [[nodiscard]] bool hasScheduledWork() const { return !m_scheduler.empty() || m_commands.hasPending(); }
    // Requires hasScheduledWork().
    [[nodiscard]] SimTime nextStepTime() const;
    [[nodiscard]] u64 stepCount() const { return m_stats.steps; }
    [[nodiscard]] u64 systemRunCount() const { return m_stats.systemRuns; }
    [[nodiscard]] const SimulationStats& stats() const { return m_stats; }
    [[nodiscard]] u64 seed() const { return m_config.seed; }
    [[nodiscard]] const SimClock& clock() const { return m_clock; }
    [[nodiscard]] const SystemScheduler& scheduler() const { return m_scheduler; }
    [[nodiscard]] const LinearArena& scratch() const { return m_scratch; }

private:
    struct StateBlock {
        std::string name;
        std::function<void(BinaryWriter&)> save;
        std::function<void(BinaryReader&)> load;
    };
    struct PeriodChange {
        SystemId id;
        SimDuration period;
    };

    [[nodiscard]] TickContext makeContext(SimTime time, SimDuration dt, u64 runIndex);

    Config m_config;
    JobSystem& m_jobs;
    SimClock m_clock;
    SystemScheduler m_scheduler;
    LinearArena m_scratch;
    World m_world;
    EventBus m_events;
    CommandQueue m_commands;
    std::vector<StateBlock> m_stateBlocks;
    std::vector<SystemId> m_due;
    std::vector<PeriodChange> m_deferredPeriodChanges;
    SimulationStats m_stats;
    bool m_inStep = false;
};

} // namespace gx
