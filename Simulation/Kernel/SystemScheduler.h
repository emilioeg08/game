#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gx {

class BinaryReader;
class BinaryWriter;
class EventBus;
class JobSystem;
class LinearArena;
class World;

// Fixed order of work inside one simulation step (docs/ARCHITECTURE.md, "Pipeline de un paso").
enum class TickPhase : u8 {
    Commands,             // queued external commands are applied first, then systems of this phase
    Simulation,           // parallel updates: read shared state, write only state the system owns
    Synchronization,      // merge buffered cross-entity results (e.g. trade flows)
    EventResolution,      // event subscribers run first, then systems of this phase
    History,              // record noteworthy events
    PresentationSnapshot, // publish read-only state for rendering/UI
    Count
};

[[nodiscard]] const char* toString(TickPhase phase);

struct TickContext {
    SimTime now;    // instant being simulated
    SimDuration dt; // time since this system last ran (zero for commands and event handlers)
    u64 step;       // kernel step index
    u64 runIndex;   // times this system ran before (key for per-run random streams)
    u64 worldSeed;
    JobSystem& jobs;
    LinearArena& scratch; // reset at the start of every step
    World& world;
    EventBus& events;
};

using SystemUpdateFn = std::function<void(const TickContext&)>;

struct SystemDesc {
    std::string name; // unique; save files refer to systems by name
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

    // Changes the rate of a system (logical LOD switch). The next run happens one new period after the last
    // run (or one new period from `now` if that is already past), and it receives the real elapsed dt, so no
    // simulated time is lost or counted twice.
    void setPeriod(SystemId id, SimDuration period, SimTime now);

    [[nodiscard]] SystemId find(std::string_view name) const;
    [[nodiscard]] const SystemState& system(SystemId id) const { return m_systems[id]; }

    // Saves the schedule of every system by name. Loading requires the same set of systems (same names and
    // phases) to be registered; periods, offsets and schedule positions come from the save.
    void write(BinaryWriter& writer) const;
    void read(BinaryReader& reader);

private:
    std::vector<SystemState> m_systems;
    std::vector<SystemId> m_executionOrder; // sorted by (phase, id)
};

} // namespace gx
