#pragma once

#include "Engine/Core/Platform.h"

namespace gx {

// Wall-clock interval timer for measurement. Never use it to drive simulation state.
class Stopwatch {
public:
    Stopwatch() : m_startNs(platform::monotonicNanoseconds()) {}

    void restart() { m_startNs = platform::monotonicNanoseconds(); }
    [[nodiscard]] u64 elapsedNs() const { return platform::monotonicNanoseconds() - m_startNs; }
    [[nodiscard]] f64 elapsedMs() const { return static_cast<f64>(elapsedNs()) / 1e6; }
    [[nodiscard]] f64 elapsedSeconds() const { return static_cast<f64>(elapsedNs()) / 1e9; }

private:
    u64 m_startNs;
};

} // namespace gx
