#include "Engine/Core/Log.h"

#include "Engine/Core/Platform.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace gx {
namespace {

constexpr const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};

struct LoggerState {
    std::mutex mutex; // guards sinks
    std::vector<std::shared_ptr<LogSink>> sinks;
    std::atomic<LogLevel> level{LogLevel::Info};
    u64 startNs = platform::monotonicNanoseconds();

    LoggerState() { sinks.push_back(std::make_shared<ConsoleLogSink>()); }
};

LoggerState& loggerState() {
    static LoggerState state;
    return state;
}

char toUpperAscii(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

} // namespace

const char* toString(LogLevel level) {
    return kLevelNames[static_cast<usize>(level)];
}

bool parseLogLevel(std::string_view text, LogLevel& out) {
    for (usize i = 0; i < std::size(kLevelNames); ++i) {
        const std::string_view name = kLevelNames[i];
        if (text.size() == name.size() && std::equal(text.begin(), text.end(), name.begin(),
                                                     [](char a, char b) { return toUpperAscii(a) == b; })) {
            out = static_cast<LogLevel>(i);
            return true;
        }
    }
    return false;
}

std::string formatLogRecord(const LogRecord& record) {
    return std::format("[{:>10.3f}] [{:<5}] [{}] [{}] {}\n", static_cast<f64>(record.timeNs) / 1e9,
                       toString(record.level), record.threadName, record.channel, record.message);
}

void ConsoleLogSink::write(const LogRecord& record) {
    const std::string line = formatLogRecord(record);
    std::fwrite(line.data(), 1, line.size(), stderr);
}

void ConsoleLogSink::flush() {
    std::fflush(stderr);
}

FileLogSink::FileLogSink(const std::filesystem::path& path)
    : m_file(std::make_unique<std::ofstream>(path, std::ios::out | std::ios::trunc)) {}

FileLogSink::~FileLogSink() = default;

bool FileLogSink::isOpen() const {
    return m_file && m_file->is_open();
}

void FileLogSink::write(const LogRecord& record) {
    if (!isOpen()) {
        return;
    }
    *m_file << formatLogRecord(record);
    if (record.level >= LogLevel::Warn) {
        m_file->flush();
    }
}

void FileLogSink::flush() {
    if (isOpen()) {
        m_file->flush();
    }
}

void MemoryLogSink::write(const LogRecord& record) {
    std::lock_guard lock(m_mutex);
    m_lines.push_back(formatLogRecord(record));
}

std::vector<std::string> MemoryLogSink::lines() const {
    std::lock_guard lock(m_mutex);
    return m_lines;
}

void MemoryLogSink::clear() {
    std::lock_guard lock(m_mutex);
    m_lines.clear();
}

namespace logging {

void setLevel(LogLevel lvl) {
    loggerState().level.store(lvl, std::memory_order_relaxed);
}

LogLevel level() {
    return loggerState().level.load(std::memory_order_relaxed);
}

void addSink(std::shared_ptr<LogSink> sink) {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    state.sinks.push_back(std::move(sink));
}

void removeSink(const LogSink* sink) {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    std::erase_if(state.sinks, [sink](const std::shared_ptr<LogSink>& s) { return s.get() == sink; });
}

void clearSinks() {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    state.sinks.clear();
}

void write(LogLevel lvl, std::string_view channel, std::string_view message) {
    if (!isEnabled(lvl)) {
        return;
    }
    LoggerState& state = loggerState();
    const LogRecord record{lvl, channel, message, platform::currentThreadName(),
                           platform::monotonicNanoseconds() - state.startNs};
    std::lock_guard lock(state.mutex);
    for (const auto& sink : state.sinks) {
        sink->write(record);
    }
}

void flush() {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    for (const auto& sink : state.sinks) {
        sink->flush();
    }
}

} // namespace logging
} // namespace gx
