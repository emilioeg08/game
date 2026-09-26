#pragma once

#include "Engine/Core/Types.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gx::bench {

struct Options {
    bool quick = false;
    u32 threads = 1;
    std::string outPath;
    std::string filter;
};

// Minimal JSON report: each result is a flat object of pre-encoded values.
class Report {
public:
    using Fields = std::vector<std::pair<std::string, std::string>>;

    static std::string number(f64 v) { return std::format("{:.6g}", v); }
    static std::string integer(u64 v) { return std::format("{}", v); }
    static std::string text(std::string_view v) { return std::format("\"{}\"", v); }
    static std::string boolean(bool v) { return v ? "true" : "false"; }

    void add(std::string_view benchmark, Fields fields) {
        fields.insert(fields.begin(), std::pair<std::string, std::string>("benchmark", text(benchmark)));
        m_results.push_back(std::move(fields));
    }

    bool write(const std::filesystem::path& path, const Fields& header) const {
        std::error_code error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << "{\n";
        for (const auto& [key, value] : header) {
            out << std::format("  \"{}\": {},\n", key, value);
        }
        out << "  \"results\": [\n";
        for (usize r = 0; r < m_results.size(); ++r) {
            out << "    {";
            for (usize f = 0; f < m_results[r].size(); ++f) {
                out << std::format("{}\"{}\": {}", f == 0 ? "" : ", ", m_results[r][f].first,
                                   m_results[r][f].second);
            }
            out << (r + 1 < m_results.size() ? "},\n" : "}\n");
        }
        out << "  ]\n}\n";
        return static_cast<bool>(out);
    }

private:
    std::vector<Fields> m_results;
};

inline void section(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::fflush(stdout);
}

// Engine
void benchProfilerOverhead(Report& report, const Options& options);
void benchJobDispatch(Report& report, const Options& options);
void benchJobScaling(Report& report, const Options& options);
// Simulation
void benchSimulationScaling(Report& report, const Options& options);
void benchGrain(Report& report, const Options& options);
void benchSaveLoad(Report& report, const Options& options);
void benchSandbox(Report& report, const Options& options);
// World
void benchEntityStorage(Report& report, const Options& options);

} // namespace gx::bench
