#pragma once

#include "Engine/Core/Platform.h"
#include "Engine/Core/Types.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef GX_ENABLE_PROFILING
#define GX_ENABLE_PROFILING 1
#endif

// Scoped CPU zones recorded into per-thread buffers.
//
// Zone names must have static storage duration (string literals, __func__, or persistentName()): buffers
// store the pointer and resolve it to a name only when collected.
//
// collect() merges the buffers into per-name aggregates (and, if enabled, into a bounded list of events for
// a Chrome/Perfetto trace). The simulation kernel calls it at the end of every step, when no zone is open.
namespace gx::profiling {

struct ZoneSummary {
    std::string name;
    u64 count = 0;
    u64 totalNs = 0;
    u64 minNs = 0;
    u64 maxNs = 0;

    [[nodiscard]] f64 meanNs() const {
        return count == 0 ? 0.0 : static_cast<f64>(totalNs) / static_cast<f64>(count);
    }
};

namespace detail {
extern std::atomic<bool> g_enabled;
void recordZone(const char* name, u64 startNs, u64 endNs);
} // namespace detail

// Runtime switch (default: on). Disabled zones cost one relaxed atomic load.
void setEnabled(bool enabled);
[[nodiscard]] inline bool isEnabled() {
    return detail::g_enabled.load(std::memory_order_relaxed);
}

// Keeps individual zone events (at most maxEvents, the rest are counted as dropped) for writeChromeTrace.
void setTraceCapture(bool enabled, usize maxEvents = 1'000'000);

// Returns a pointer with static lifetime for a dynamic name (e.g. a system name); equal text, equal pointer.
[[nodiscard]] const char* persistentName(std::string_view name);

// Drains per-thread buffers into the aggregates. Cheap when nothing was recorded.
void collect();

// Aggregates since the last resetStats(), sorted by total time, descending. Collects first.
[[nodiscard]] std::vector<ZoneSummary> summary();
// Points into `zones`, so it must be a named vector that outlives the result (temporaries are rejected).
[[nodiscard]] const ZoneSummary* findZone(const std::vector<ZoneSummary>& zones, std::string_view name);
const ZoneSummary* findZone(std::vector<ZoneSummary>&& zones, std::string_view name) = delete;

// Discards aggregates, captured trace events and unprocessed records.
void resetStats();

// Chrome trace event format; open in https://ui.perfetto.dev or chrome://tracing.
bool writeChromeTrace(const std::filesystem::path& path);
[[nodiscard]] u64 droppedTraceEvents();

[[nodiscard]] std::string formatSummaryTable(const std::vector<ZoneSummary>& zones);

class ScopedZone {
public:
    explicit ScopedZone(const char* name) : m_name(isEnabled() ? name : nullptr) {
        if (m_name != nullptr) {
            m_startNs = platform::monotonicNanoseconds();
        }
    }
    ~ScopedZone() {
        if (m_name != nullptr) {
            detail::recordZone(m_name, m_startNs, platform::monotonicNanoseconds());
        }
    }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    const char* m_name;
    u64 m_startNs = 0;
};

} // namespace gx::profiling

#if GX_ENABLE_PROFILING
#define GX_PROFILE_SCOPE(name) const ::gx::profiling::ScopedZone GX_CONCAT(gxProfileZone_, __LINE__)(name)
#else
#define GX_PROFILE_SCOPE(name) ((void)0)
#endif
#define GX_PROFILE_FUNCTION() GX_PROFILE_SCOPE(__func__)
