#include "Engine/Profiling/Statistics.h"

#include <algorithm>
#include <cmath>

namespace gx {
namespace {

f64 percentile(const std::vector<f64>& sorted, f64 p) {
    const auto rank = static_cast<usize>(std::ceil(p * static_cast<f64>(sorted.size())));
    return sorted[std::clamp<usize>(rank, 1, sorted.size()) - 1];
}

} // namespace

SampleStats computeSampleStats(std::vector<f64> samples) {
    SampleStats stats;
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    stats.count = samples.size();
    stats.min = samples.front();
    stats.max = samples.back();

    f64 sum = 0.0;
    for (const f64 s : samples) {
        sum += s;
    }
    stats.mean = sum / static_cast<f64>(samples.size());

    f64 squares = 0.0;
    for (const f64 s : samples) {
        squares += (s - stats.mean) * (s - stats.mean);
    }
    stats.stddev = std::sqrt(squares / static_cast<f64>(samples.size()));

    stats.p50 = percentile(samples, 0.50);
    stats.p95 = percentile(samples, 0.95);
    stats.p99 = percentile(samples, 0.99);
    return stats;
}

} // namespace gx
