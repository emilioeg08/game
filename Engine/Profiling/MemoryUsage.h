#pragma once

#include "Engine/Core/Types.h"

namespace gx {

struct ProcessMemory {
    u64 workingSetBytes = 0;     // resident memory
    u64 peakWorkingSetBytes = 0; // resident high-water mark
    u64 privateBytes = 0;        // committed private memory (Windows) / data segment (Linux)
};

// Zeroes where the platform does not expose a value.
[[nodiscard]] ProcessMemory queryProcessMemory();

[[nodiscard]] inline f64 toMiB(u64 bytes) {
    return static_cast<f64>(bytes) / (1024.0 * 1024.0);
}

} // namespace gx
