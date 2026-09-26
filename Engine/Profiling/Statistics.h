#pragma once

#include "Engine/Core/Types.h"

#include <vector>

namespace gx {

struct SampleStats {
    usize count = 0;
    f64 mean = 0.0;
    f64 stddev = 0.0;
    f64 min = 0.0;
    f64 max = 0.0;
    f64 p50 = 0.0;
    f64 p95 = 0.0;
    f64 p99 = 0.0;
};

// Nearest-rank percentiles. Takes the samples by value because it sorts them.
[[nodiscard]] SampleStats computeSampleStats(std::vector<f64> samples);

} // namespace gx
