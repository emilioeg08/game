#include "Engine/Time/TimeController.h"

#include "Engine/Core/Assert.h"

namespace gx {

TimeController::TimeController(SimTime start) : TimeController(start, Config{}) {}

TimeController::TimeController(SimTime start, const Config& config) : m_config(config), m_target(start) {}

void TimeController::setSpeed(u32 multiplier) {
    GX_CHECK(multiplier >= 1, "time speed must be at least 1x");
    m_speed = multiplier;
}

SimTime TimeController::update(u64 realDeltaNs, SimTime simNow) {
    if (m_target < simNow) {
        m_target = simNow;
    }
    if (m_paused) {
        m_target = simNow; // freeze immediately; pending catch-up is discarded
        m_carryNs = 0;
        return m_target;
    }

    const u64 scaledNs = realDeltaNs * m_speed + m_carryNs;
    m_carryNs = scaledNs % 1'000;
    m_target += SimDuration::microseconds(static_cast<i64>(scaledNs / 1'000));

    const SimDuration maxLag =
        SimDuration::microseconds(static_cast<i64>(m_config.maxRealLagNs / 1'000 * m_speed));
    if (m_target - simNow > maxLag) {
        m_dropped += (m_target - simNow) - maxLag;
        m_target = simNow + maxLag;
    }
    return m_target;
}

} // namespace gx
