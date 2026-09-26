#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Time/SimTime.h"

namespace gx {

// Authoritative simulation time. Only the simulation kernel advances it, and never backwards.
class SimClock {
public:
    explicit SimClock(SimTime start = SimTime::epoch()) : m_start(start), m_now(start) {}

    [[nodiscard]] SimTime now() const { return m_now; }
    [[nodiscard]] SimTime start() const { return m_start; }
    [[nodiscard]] SimDuration elapsed() const { return m_now - m_start; }

    void advanceTo(SimTime time) {
        GX_CHECK(time >= m_now, "simulation time cannot go backwards");
        m_now = time;
    }

private:
    SimTime m_start;
    SimTime m_now;
};

} // namespace gx
