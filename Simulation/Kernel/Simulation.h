#pragma once

#include "Engine/Memory/LinearArena.h"
#include "Engine/Time/SimClock.h"
#include "Simulation/Kernel/SystemScheduler.h"

#include <vector>

namespace gx {

class JobSystem;

// Headless simulation kernel: owns the clock and the system schedule and advances them step by step.
// Needs no renderer, UI or wall clock; interactive pacing lives outside (TimeController).
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

    // Not allowed while a step is running (fails a GX_CHECK and returns kInvalidSystemId).
    SystemId addSystem(SystemDesc desc);

    // Advances to the next scheduled instant and runs every system due there. False if no systems exist.
    bool step();

    // Runs every step scheduled at or before `target`, then moves the clock to `target`.
    // With a non-zero wall budget the call may return early (clock at the last executed step); calling again
    // resumes exactly where it stopped, so results never depend on how the run is sliced.
    u64 runUntil(SimTime target, u64 wallBudgetNs = 0);
    u64 runFor(SimDuration duration) { return runUntil(now() + duration); }

    [[nodiscard]] SimTime now() const { return m_clock.now(); }
    // Requires at least one system.
    [[nodiscard]] SimTime nextStepTime() const { return m_scheduler.nextDueTime(); }
    [[nodiscard]] u64 stepCount() const { return m_stepCount; }
    [[nodiscard]] u64 systemRunCount() const { return m_systemRuns; }
    [[nodiscard]] u64 seed() const { return m_config.seed; }
    [[nodiscard]] const SimClock& clock() const { return m_clock; }
    [[nodiscard]] const SystemScheduler& scheduler() const { return m_scheduler; }
    [[nodiscard]] const LinearArena& scratch() const { return m_scratch; }
    [[nodiscard]] JobSystem& jobs() { return m_jobs; }

private:
    Config m_config;
    JobSystem& m_jobs;
    SimClock m_clock;
    SystemScheduler m_scheduler;
    LinearArena m_scratch;
    std::vector<SystemId> m_due;
    u64 m_stepCount = 0;
    u64 m_systemRuns = 0;
    bool m_inStep = false;
};

} // namespace gx
