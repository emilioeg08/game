#include "Simulation/Kernel/Simulation.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Platform.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/Binary.h"

#include <format>

namespace gx {
namespace {

constexpr u32 kKernelChunk = fourCC("KRNL");
constexpr u32 kScheduleChunk = fourCC("SCHD");
constexpr u32 kCommandsChunk = fourCC("CMDQ");
constexpr u32 kWorldChunk = fourCC("WRLD");
constexpr u32 kBlocksChunk = fourCC("BLKS");
constexpr u32 kBlockChunk = fourCC("BLCK");
constexpr u32 kChunkVersion = 1;

} // namespace

Simulation::Simulation(const Config& config, JobSystem& jobs)
    : m_config(config), m_jobs(jobs), m_clock(config.startTime), m_scratch(config.scratchBytes) {}

SystemId Simulation::addSystem(SystemDesc desc) {
    // Adding while systems run could reallocate the storage of the function being executed.
    GX_CHECK(!m_inStep, "systems cannot be added during a step ('{}')", desc.name);
    if (m_inStep) {
        return kInvalidSystemId;
    }
    return m_scheduler.add(std::move(desc), m_clock.now());
}

void Simulation::addStateBlock(std::string name, std::function<void(BinaryWriter&)> save,
                               std::function<void(BinaryReader&)> load) {
    GX_CHECK(!m_inStep, "state blocks cannot be added during a step ('{}')", name);
    GX_CHECK(save && load, "state block '{}' needs save and load functions", name);
    for (const StateBlock& block : m_stateBlocks) {
        GX_CHECK(block.name != name, "state block '{}' registered twice", name);
    }
    m_stateBlocks.push_back({std::move(name), std::move(save), std::move(load)});
}

TickContext Simulation::makeContext(SimTime time, SimDuration dt, u64 runIndex) {
    return {time, dt, m_stats.steps, runIndex, m_config.seed, m_jobs, m_scratch, m_world, m_events};
}

SimTime Simulation::nextStepTime() const {
    GX_ASSERT(hasScheduledWork(), "nothing is scheduled");
    if (m_scheduler.empty()) {
        return m_commands.nextTime();
    }
    if (!m_commands.hasPending()) {
        return m_scheduler.nextDueTime();
    }
    return std::min(m_scheduler.nextDueTime(), m_commands.nextTime());
}

bool Simulation::step() {
    if (!hasScheduledWork()) {
        return false;
    }
    {
        GX_PROFILE_SCOPE("Sim.Step");
        m_inStep = true;
        const SimTime now = nextStepTime();
        m_clock.advanceTo(now);
        m_scratch.reset();
        m_scheduler.collectDue(now, m_due);

        if (m_commands.hasPending() && m_commands.nextTime() <= now) {
            GX_PROFILE_SCOPE("Sim.Commands");
            m_stats.commandsApplied += m_commands.applyDue(now, makeContext(now, {}, 0));
        }

        bool eventsDispatched = false;
        const auto dispatchEvents = [&] {
            eventsDispatched = true;
            if (m_events.channelCount() == 0) {
                return;
            }
            GX_PROFILE_SCOPE("Sim.EventDispatch");
            m_events.dispatch(makeContext(now, {}, 0));
        };
        for (const SystemId id : m_due) {
            const SystemState& entry = m_scheduler.system(id);
            if (!eventsDispatched && entry.desc.phase >= TickPhase::EventResolution) {
                dispatchEvents();
            }
            const TickContext context = makeContext(now, now - entry.lastRun, entry.runCount);
            {
                GX_PROFILE_SCOPE(entry.profileName);
                entry.desc.update(context);
            }
            m_scheduler.markRan(id, now);
        }
        if (!eventsDispatched) {
            dispatchEvents();
        }

        for (const PeriodChange& change : m_deferredPeriodChanges) {
            m_scheduler.setPeriod(change.id, change.period, now);
        }
        m_deferredPeriodChanges.clear();

        m_stats.eventsEmitted += m_events.clearAll();
        m_stats.systemRuns += m_due.size();
        ++m_stats.steps;
        m_inStep = false;
    }
    // Every job of this step has completed: no zone is open on any thread.
    profiling::collect();
    return true;
}

u64 Simulation::runUntil(SimTime target, u64 wallBudgetNs) {
    GX_CHECK(target >= m_clock.now(), "runUntil target is in the past");
    const u64 startNs = wallBudgetNs != 0 ? platform::monotonicNanoseconds() : 0;
    u64 executed = 0;
    while (hasScheduledWork() && nextStepTime() <= target) {
        step();
        ++executed;
        if (wallBudgetNs != 0 && platform::monotonicNanoseconds() - startNs >= wallBudgetNs) {
            if (hasScheduledWork() && nextStepTime() <= target) {
                return executed; // out of budget: resume on the next call
            }
            break;
        }
    }
    m_clock.advanceTo(target);
    return executed;
}

void Simulation::setSystemPeriod(SystemId id, SimDuration period) {
    GX_CHECK(id < m_scheduler.size(), "invalid system id {}", id);
    GX_CHECK(period > SimDuration{}, "system period must be positive");
    if (m_inStep) {
        m_deferredPeriodChanges.push_back({id, period});
    } else {
        m_scheduler.setPeriod(id, period, m_clock.now());
    }
}

std::vector<std::byte> Simulation::saveState() const {
    GX_CHECK(!m_inStep, "the simulation cannot be saved during a step");
    BinaryWriter writer;
    {
        const auto mark = writer.beginChunk(kKernelChunk, kChunkVersion);
        writer.io(m_config.seed);
        writer.io(m_clock.start());
        writer.io(m_clock.now());
        writer.io(m_stats.steps);
        writer.io(m_stats.systemRuns);
        writer.io(m_stats.commandsApplied);
        writer.io(m_stats.eventsEmitted);
        writer.endChunk(mark);
    }
    {
        const auto mark = writer.beginChunk(kScheduleChunk, kChunkVersion);
        m_scheduler.write(writer);
        writer.endChunk(mark);
    }
    {
        const auto mark = writer.beginChunk(kCommandsChunk, kChunkVersion);
        m_commands.write(writer);
        writer.endChunk(mark);
    }
    {
        const auto mark = writer.beginChunk(kWorldChunk, kChunkVersion);
        m_world.write(writer);
        writer.endChunk(mark);
    }
    {
        const auto mark = writer.beginChunk(kBlocksChunk, kChunkVersion);
        writer.writeU32(static_cast<u32>(m_stateBlocks.size()));
        for (const StateBlock& block : m_stateBlocks) {
            const auto blockMark = writer.beginChunk(kBlockChunk, kChunkVersion);
            writer.writeString(block.name);
            block.save(writer);
            writer.endChunk(blockMark);
        }
        writer.endChunk(mark);
    }
    return writer.takeBytes();
}

bool Simulation::loadState(std::span<const std::byte> bytes, std::string& error) {
    GX_CHECK(!m_inStep, "the simulation cannot be loaded during a step");
    BinaryReader reader(bytes);
    BinaryReader::Chunk chunk;

    if (reader.beginChunk(kKernelChunk, chunk)) {
        SimTime start;
        SimTime now;
        SimulationStats stats;
        reader.io(m_config.seed);
        reader.io(start);
        reader.io(now);
        reader.io(stats.steps);
        reader.io(stats.systemRuns);
        reader.io(stats.commandsApplied);
        reader.io(stats.eventsEmitted);
        if (reader.ok() && now < start) {
            reader.fail("saved clock is before its start");
        }
        if (reader.ok()) {
            m_clock.restore(start, now);
            m_stats = stats;
        }
        reader.endChunk(chunk);
    }
    if (reader.ok() && reader.beginChunk(kScheduleChunk, chunk)) {
        m_scheduler.read(reader);
        reader.endChunk(chunk);
    }
    if (reader.ok() && reader.beginChunk(kCommandsChunk, chunk)) {
        m_commands.read(reader);
        reader.endChunk(chunk);
    }
    if (reader.ok() && reader.beginChunk(kWorldChunk, chunk)) {
        m_world.read(reader);
        reader.endChunk(chunk);
    }
    if (reader.ok() && reader.beginChunk(kBlocksChunk, chunk)) {
        const u32 blockCount = reader.readU32();
        if (reader.ok() && blockCount != m_stateBlocks.size()) {
            reader.fail(std::format("save has {} state blocks, this build registers {}", blockCount,
                                    m_stateBlocks.size()));
        }
        for (u32 i = 0; i < blockCount && reader.ok(); ++i) {
            BinaryReader::Chunk blockChunk;
            if (!reader.beginChunk(kBlockChunk, blockChunk)) {
                break;
            }
            const std::string name = reader.readString();
            // Blocks are matched by position: registration order is part of the structure.
            if (reader.ok() && name != m_stateBlocks[i].name) {
                reader.fail(
                    std::format("expected state block '{}' but found '{}'", m_stateBlocks[i].name, name));
                break;
            }
            m_stateBlocks[i].load(reader);
            reader.endChunk(blockChunk);
        }
        reader.endChunk(chunk);
    }
    if (reader.ok() && reader.remaining() != 0) {
        reader.fail("unexpected data after the last chunk");
    }
    if (reader.ok() && hasScheduledWork() && nextStepTime() <= m_clock.now()) {
        reader.fail("saved schedule has work due in the past");
    }
    if (!reader.ok()) {
        error = reader.error();
        return false;
    }
    return true;
}

u64 Simulation::stateHash() const {
    return hashBytes(saveState());
}

} // namespace gx
