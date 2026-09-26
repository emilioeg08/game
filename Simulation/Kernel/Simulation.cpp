#include "Simulation/Kernel/Simulation.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Platform.h"
#include "Engine/Profiling/Profiler.h"

namespace gx {

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

bool Simulation::step() {
    if (m_scheduler.empty()) {
        return false;
    }
    {
        GX_PROFILE_SCOPE("Sim.Step");
        m_inStep = true;
        const SimTime now = m_scheduler.nextDueTime();
        m_clock.advanceTo(now);
        m_scratch.reset();
        m_scheduler.collectDue(now, m_due);
        for (const SystemId id : m_due) {
            const SystemState& entry = m_scheduler.system(id);
            const TickContext context{
                now, now - entry.lastRun, m_stepCount, entry.runCount, m_config.seed, m_jobs, m_scratch};
            {
                GX_PROFILE_SCOPE(entry.profileName);
                entry.desc.update(context);
            }
            m_scheduler.markRan(id, now);
        }
        m_systemRuns += m_due.size();
        ++m_stepCount;
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
    while (!m_scheduler.empty() && m_scheduler.nextDueTime() <= target) {
        step();
        ++executed;
        if (wallBudgetNs != 0 && platform::monotonicNanoseconds() - startNs >= wallBudgetNs) {
            if (m_scheduler.nextDueTime() <= target) {
                return executed; // out of budget: resume on the next call
            }
            break;
        }
    }
    m_clock.advanceTo(target);
    return executed;
}

} // namespace gx
