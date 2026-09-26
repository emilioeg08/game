#include "Simulation/Kernel/SystemScheduler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Profiling/Profiler.h"

#include <algorithm>

namespace gx {

const char* toString(TickPhase phase) {
    switch (phase) {
    case TickPhase::Commands:
        return "Commands";
    case TickPhase::Simulation:
        return "Simulation";
    case TickPhase::Synchronization:
        return "Synchronization";
    case TickPhase::EventResolution:
        return "EventResolution";
    case TickPhase::History:
        return "History";
    case TickPhase::PresentationSnapshot:
        return "PresentationSnapshot";
    case TickPhase::Count:
        break;
    }
    return "Unknown";
}

SystemId SystemScheduler::add(SystemDesc desc, SimTime now) {
    GX_CHECK(desc.period > SimDuration{}, "system '{}' needs a positive period", desc.name);
    GX_CHECK(desc.offset >= SimDuration{} && desc.offset < desc.period,
             "system '{}': offset must be in [0, period)", desc.name);
    GX_CHECK(static_cast<bool>(desc.update), "system '{}' has no update function", desc.name);
    GX_CHECK(desc.phase < TickPhase::Count, "system '{}' has an invalid phase", desc.name);

    const auto id = static_cast<SystemId>(m_systems.size());
    SystemState state;
    state.profileName = profiling::persistentName(desc.name);
    state.lastRun = now;
    state.nextDue = now + (desc.offset > SimDuration{} ? desc.offset : desc.period);
    state.desc = std::move(desc);
    m_systems.push_back(std::move(state));

    m_executionOrder.push_back(id);
    std::stable_sort(m_executionOrder.begin(), m_executionOrder.end(), [this](SystemId a, SystemId b) {
        return m_systems[a].desc.phase < m_systems[b].desc.phase;
    });
    return id;
}

SimTime SystemScheduler::nextDueTime() const {
    GX_ASSERT(!m_systems.empty(), "no systems registered");
    SimTime earliest = m_systems.front().nextDue;
    for (const SystemState& s : m_systems) {
        earliest = std::min(earliest, s.nextDue);
    }
    return earliest;
}

void SystemScheduler::collectDue(SimTime time, std::vector<SystemId>& out) const {
    out.clear();
    for (const SystemId id : m_executionOrder) {
        GX_ASSERT(m_systems[id].nextDue >= time, "system '{}' missed its due time", m_systems[id].desc.name);
        if (m_systems[id].nextDue == time) {
            out.push_back(id);
        }
    }
}

void SystemScheduler::markRan(SystemId id, SimTime time) {
    SystemState& s = m_systems[id];
    GX_ASSERT(s.nextDue == time, "system '{}' ran off its schedule", s.desc.name);
    s.lastRun = time;
    s.nextDue += s.desc.period; // stays on the grid: no accumulated drift
    ++s.runCount;
}

} // namespace gx
