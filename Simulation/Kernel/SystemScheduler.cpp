#include "Simulation/Kernel/SystemScheduler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/Binary.h"

#include <algorithm>
#include <format>

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
    GX_CHECK(find(desc.name) == kInvalidSystemId, "system name '{}' is already registered", desc.name);

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

void SystemScheduler::setPeriod(SystemId id, SimDuration period, SimTime now) {
    GX_CHECK(id < m_systems.size(), "invalid system id {}", id);
    GX_CHECK(period > SimDuration{}, "system period must be positive");
    SystemState& s = m_systems[id];
    s.desc.period = period;
    s.desc.offset = {};
    s.nextDue = s.lastRun + period;
    if (s.nextDue <= now) {
        s.nextDue = now + period;
    }
}

SystemId SystemScheduler::find(std::string_view name) const {
    for (usize i = 0; i < m_systems.size(); ++i) {
        if (m_systems[i].desc.name == name) {
            return static_cast<SystemId>(i);
        }
    }
    return kInvalidSystemId;
}

void SystemScheduler::write(BinaryWriter& writer) const {
    writer.writeU32(static_cast<u32>(m_systems.size()));
    for (const SystemState& s : m_systems) {
        writer.writeString(s.desc.name);
        writer.io(s.desc.phase);
        writer.io(s.desc.period);
        writer.io(s.desc.offset);
        writer.io(s.lastRun);
        writer.io(s.nextDue);
        writer.io(s.runCount);
    }
}

void SystemScheduler::read(BinaryReader& reader) {
    const u32 count = reader.readU32();
    if (reader.ok() && count != m_systems.size()) {
        reader.fail(std::format("save has {} systems, this build registers {}", count, m_systems.size()));
        return;
    }
    std::vector<SystemState> restored = m_systems;
    std::vector<u8> seen(m_systems.size(), 0);
    for (u32 i = 0; i < count && reader.ok(); ++i) {
        const std::string name = reader.readString();
        TickPhase phase{};
        SimDuration period;
        SimDuration offset;
        SimTime lastRun;
        SimTime nextDue;
        u64 runCount = 0;
        reader.io(phase);
        reader.io(period);
        reader.io(offset);
        reader.io(lastRun);
        reader.io(nextDue);
        reader.io(runCount);
        if (!reader.ok()) {
            return;
        }
        const SystemId id = find(name);
        if (id == kInvalidSystemId || seen[id] != 0) {
            reader.fail(std::format("save has unknown or repeated system '{}'", name));
            return;
        }
        if (phase != m_systems[id].desc.phase || period <= SimDuration{} || nextDue <= lastRun) {
            reader.fail(std::format("saved schedule of system '{}' is inconsistent with this build", name));
            return;
        }
        seen[id] = 1;
        SystemState& s = restored[id];
        s.desc.period = period;
        s.desc.offset = offset;
        s.lastRun = lastRun;
        s.nextDue = nextDue;
        s.runCount = runCount;
    }
    if (reader.ok()) {
        m_systems = std::move(restored);
    }
}

} // namespace gx
