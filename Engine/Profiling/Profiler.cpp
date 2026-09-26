#include "Engine/Profiling/Profiler.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace gx::profiling {

namespace detail {
std::atomic<bool> g_enabled{true};
} // namespace detail

namespace {

struct RawZone {
    const char* name;
    u64 startNs;
    u64 endNs;
};

struct ThreadBuffer {
    std::mutex mutex; // the owning thread appends, collect() drains
    std::vector<RawZone> zones;
    std::string threadName;
    u32 traceThreadId = 0;
};

struct Aggregate {
    u64 count = 0;
    u64 totalNs = 0;
    u64 minNs = std::numeric_limits<u64>::max();
    u64 maxNs = 0;
};

struct TraceEvent {
    u32 nameId;
    u32 traceThreadId;
    u64 startNs;
    u64 durationNs;
};

struct ProfilerState {
    std::mutex mutex; // guards everything below except the persistent names
    std::vector<std::unique_ptr<ThreadBuffer>> threads;
    std::unordered_map<const char*, u32> idByPointer; // valid because names have static lifetime
    std::unordered_map<std::string, u32> idByName;
    std::vector<std::string> names;
    std::vector<Aggregate> aggregates; // indexed by name id
    std::vector<RawZone> scratch;
    bool traceEnabled = false;
    usize traceMaxEvents = 0;
    std::vector<TraceEvent> trace;
    u64 traceDropped = 0;

    std::mutex persistentMutex;
    std::unordered_set<std::string> persistentNames; // node-based: element addresses never change
};

ProfilerState& state() {
    // Intentionally leaked: zones may still be recorded while other statics are being destroyed.
    static auto* instance = new ProfilerState();
    return *instance;
}

thread_local ThreadBuffer* t_buffer = nullptr;

ThreadBuffer& threadBuffer() {
    if (t_buffer == nullptr) {
        auto buffer = std::make_unique<ThreadBuffer>();
        buffer->threadName = platform::currentThreadName();
        ProfilerState& s = state();
        std::lock_guard lock(s.mutex);
        buffer->traceThreadId = static_cast<u32>(s.threads.size());
        t_buffer = buffer.get();
        s.threads.push_back(std::move(buffer));
    }
    return *t_buffer;
}

u32 internNameLocked(ProfilerState& s, const char* name) {
    if (const auto it = s.idByPointer.find(name); it != s.idByPointer.end()) {
        return it->second;
    }
    const auto [it, inserted] = s.idByName.try_emplace(name, static_cast<u32>(s.names.size()));
    if (inserted) {
        s.names.emplace_back(name);
        s.aggregates.emplace_back();
    }
    s.idByPointer.emplace(name, it->second);
    return it->second;
}

void collectLocked(ProfilerState& s) {
    for (const auto& buffer : s.threads) {
        {
            std::lock_guard bufferLock(buffer->mutex);
            if (buffer->zones.empty()) {
                continue;
            }
            s.scratch.swap(buffer->zones); // the buffer keeps the scratch vector's capacity
        }
        for (const RawZone& zone : s.scratch) {
            const u32 id = internNameLocked(s, zone.name);
            const u64 duration = zone.endNs - zone.startNs;
            Aggregate& aggregate = s.aggregates[id];
            ++aggregate.count;
            aggregate.totalNs += duration;
            aggregate.minNs = std::min(aggregate.minNs, duration);
            aggregate.maxNs = std::max(aggregate.maxNs, duration);
            if (s.traceEnabled) {
                if (s.trace.size() < s.traceMaxEvents) {
                    s.trace.push_back({id, buffer->traceThreadId, zone.startNs, duration});
                } else {
                    ++s.traceDropped;
                }
            }
        }
        s.scratch.clear();
    }
}

std::string escapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // namespace

namespace detail {

void recordZone(const char* name, u64 startNs, u64 endNs) {
    ThreadBuffer& buffer = threadBuffer();
    std::lock_guard lock(buffer.mutex);
    buffer.zones.push_back({name, startNs, endNs});
}

} // namespace detail

void setEnabled(bool enabled) {
    detail::g_enabled.store(enabled, std::memory_order_relaxed);
}

void setTraceCapture(bool enabled, usize maxEvents) {
    ProfilerState& s = state();
    std::lock_guard lock(s.mutex);
    s.traceEnabled = enabled;
    s.traceMaxEvents = maxEvents;
    if (enabled) {
        s.trace.reserve(std::min<usize>(maxEvents, 1u << 16));
    }
}

const char* persistentName(std::string_view name) {
    ProfilerState& s = state();
    std::lock_guard lock(s.persistentMutex);
    return s.persistentNames.emplace(name).first->c_str();
}

void collect() {
    ProfilerState& s = state();
    std::lock_guard lock(s.mutex);
    collectLocked(s);
}

std::vector<ZoneSummary> summary() {
    ProfilerState& s = state();
    std::lock_guard lock(s.mutex);
    collectLocked(s);
    std::vector<ZoneSummary> zones;
    for (usize id = 0; id < s.aggregates.size(); ++id) {
        const Aggregate& a = s.aggregates[id];
        if (a.count > 0) {
            zones.push_back({s.names[id], a.count, a.totalNs, a.minNs, a.maxNs});
        }
    }
    std::sort(zones.begin(), zones.end(), [](const ZoneSummary& a, const ZoneSummary& b) {
        return a.totalNs != b.totalNs ? a.totalNs > b.totalNs : a.name < b.name;
    });
    return zones;
}

const ZoneSummary* findZone(const std::vector<ZoneSummary>& zones, std::string_view name) {
    const auto it =
        std::find_if(zones.begin(), zones.end(), [name](const ZoneSummary& z) { return z.name == name; });
    return it == zones.end() ? nullptr : &*it;
}

void resetStats() {
    ProfilerState& s = state();
    std::lock_guard lock(s.mutex);
    for (const auto& buffer : s.threads) {
        std::lock_guard bufferLock(buffer->mutex);
        buffer->zones.clear();
    }
    std::fill(s.aggregates.begin(), s.aggregates.end(), Aggregate{});
    s.trace.clear();
    s.traceDropped = 0;
}

bool writeChromeTrace(const std::filesystem::path& path) {
    ProfilerState& s = state();
    std::lock_guard lock(s.mutex);
    collectLocked(s);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    u64 originNs = std::numeric_limits<u64>::max();
    for (const TraceEvent& e : s.trace) {
        originNs = std::min(originNs, e.startNs);
    }

    out << "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
    bool first = true;
    const auto separator = [&] {
        if (!first) {
            out << ",\n";
        }
        first = false;
    };
    for (const auto& thread : s.threads) {
        separator();
        out << std::format(R"({{"name":"thread_name","ph":"M","pid":1,"tid":{},"args":{{"name":"{}"}}}})",
                           thread->traceThreadId, escapeJson(thread->threadName));
    }
    for (const TraceEvent& e : s.trace) {
        separator();
        out << std::format(R"({{"name":"{}","ph":"X","pid":1,"tid":{},"ts":{:.3f},"dur":{:.3f}}})",
                           escapeJson(s.names[e.nameId]), e.traceThreadId,
                           static_cast<f64>(e.startNs - originNs) / 1000.0,
                           static_cast<f64>(e.durationNs) / 1000.0);
    }
    out << "\n]}\n";
    return static_cast<bool>(out);
}

u64 droppedTraceEvents() {
    ProfilerState& s = state();
    std::lock_guard lock(s.mutex);
    return s.traceDropped;
}

std::string formatSummaryTable(const std::vector<ZoneSummary>& zones) {
    std::string out = std::format("{:<34} {:>10} {:>12} {:>12} {:>12} {:>12}\n", "Zone", "Calls", "Total ms",
                                  "Mean us", "Min us", "Max us");
    for (const ZoneSummary& z : zones) {
        out += std::format("{:<34} {:>10} {:>12.3f} {:>12.3f} {:>12.3f} {:>12.3f}\n", z.name, z.count,
                           static_cast<f64>(z.totalNs) / 1e6, z.meanNs() / 1e3,
                           static_cast<f64>(z.minNs) / 1e3, static_cast<f64>(z.maxNs) / 1e3);
    }
    return out;
}

} // namespace gx::profiling
