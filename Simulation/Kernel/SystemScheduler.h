#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"

#include <functional>
#include <string>
#include <vector>

namespace gx {

class JobSystem;
class LinearArena;

// Fixed order of work inside one simulation step (docs/ARCHITECTURE.md, "Pipeline de un paso").
enum class TickPhase : u8 {
    Commands,             // apply queued player/AI commands
    Simulation,           // parallel updates: read shared state, write only state the system owns
    Synchronization,      // merge buffered cross-entity results (e.g. trade flows)
    EventResolution,      // resolve events raised during the step
    History,              // record noteworthy events
    PresentationSnapshot, // publish read-only state for rendering/UI
    Count
};

[[nodiscard]] const char* toString(TickPhase phase);

struct TickContext {
    SimTime now;    // instant being simulated
    SimDuration dt; // time since this system last ran
    u64 step;       // kernel step index
    u64 runIndex;   // times this system ran before (key for per-run random streams)
    u64 worldSeed;
    JobSystem& jobs;
    LinearArena& scratch; // reset at the start of every step
};

using SystemUpdateFn = std::function<void(const TickContext&)>;

struct SystemDesc {
    std::string name;
    TickPhase phase = TickPhase::Simulation;
    SimDuration period = SimDuration::seconds(1);
    // Shifts the schedule within [0, period) so costly systems sharing a period can be staggered.
    SimDuration offset{};
    SystemUpdateFn update;
};

using SystemId = u32;
inline constexpr SystemId kInvalidSystemId = ~SystemId{0};

struct SystemState {
    SystemDesc desc;
    const char* profileName = nullptr; // persistent copy of desc.name for profiling zones
    SimTime lastRun;
    SimTime nextDue;
    u64 runCount = 0;
};

// Multi-rate scheduler. Each system runs on its own fixed grid (added + offset + k * period) and the kernel
// jumps straight to the next instant at which any system is due. Strategic systems (hourly economy, daily
// politics) cost nothing between runs; fast systems (tactical, sub-second) only cost while registered.
// Execution order inside an instant: phase, then registration order. Fully deterministic.
class SystemScheduler {
public:
    // `now` anchors the new system's schedule.
    SystemId add(SystemDesc desc, SimTime now);

    [[nodiscard]] bool empty() const { return m_systems.empty(); }
    [[nodiscard]] usize size() const { return m_systems.size(); }
    // Earliest due instant. Requires !empty().
    [[nodiscard]] SimTime nextDueTime() const;
    // Systems due at `time`, in execution order.
    void collectDue(SimTime time, std::vector<SystemId>& out) const;
    void markRan(SystemId id, SimTime time);

    [[nodiscard]] const SystemState& system(SystemId id) const { return m_systems[id]; }

private:
    std::vector<SystemState> m_systems;
    std::vector<SystemId> m_executionOrder; // sorted by (phase, id)
};

} // namespace gx
