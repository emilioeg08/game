#pragma once

#include "Engine/Time/SimTime.h"

namespace gx {

// Converts wall-clock time into a simulation-time target for interactive play: pause and acceleration.
//
// It only decides how far the simulation should get, never how it gets there: the kernel always steps
// through the same scheduled instants with the same dt, so any speed, frame rate or pause pattern produces
// bit-identical simulation state. Acceleration means more steps per real second, never a bigger dt.
class TimeController {
public:
    struct Config {
        // How far (in wall time) the simulation may fall behind its target before the excess is dropped.
        // Prevents a slow simulation from trying to catch up forever (effective speed drops instead).
        u64 maxRealLagNs = 250'000'000;
    };

    explicit TimeController(SimTime start);
    TimeController(SimTime start, const Config& config);

    void setPaused(bool paused) { m_paused = paused; }
    [[nodiscard]] bool isPaused() const { return m_paused; }

    // Simulated seconds per real second (1 = real time). Must be >= 1.
    void setSpeed(u32 multiplier);
    [[nodiscard]] u32 speed() const { return m_speed; }

    // Accounts for `realDeltaNs` of wall time and returns the simulation time to reach this frame.
    // `simNow` is where the simulation actually is.
    SimTime update(u64 realDeltaNs, SimTime simNow);

    // Simulation time given up because the simulation could not keep up.
    [[nodiscard]] SimDuration droppedTime() const { return m_dropped; }

private:
    Config m_config;
    SimTime m_target;
    u64 m_carryNs = 0; // sub-microsecond remainder, so no simulated time is lost to rounding
    u32 m_speed = 1;
    bool m_paused = false;
    SimDuration m_dropped;
};

} // namespace gx
