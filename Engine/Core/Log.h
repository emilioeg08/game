#pragma once

#include "Engine/Core/Types.h"

#include <filesystem>
#include <format>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gx {

enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error, Fatal, Off };

[[nodiscard]] const char* toString(LogLevel level);
// Case-insensitive ("info", "WARN", ...). Returns false for unknown names.
[[nodiscard]] bool parseLogLevel(std::string_view text, LogLevel& out);

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view channel;
    std::string_view message;
    std::string_view threadName;
    u64 timeNs = 0; // monotonic, since the logger started
};

// One line, newline-terminated: "[    12.345] [INFO ] [thread] [Channel] message".
[[nodiscard]] std::string formatLogRecord(const LogRecord& record);

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

// Writes to stderr, keeping stdout free for program output (reports, benchmark tables).
class ConsoleLogSink final : public LogSink {
public:
    void write(const LogRecord& record) override;
    void flush() override;
};

class FileLogSink final : public LogSink {
public:
    explicit FileLogSink(const std::filesystem::path& path);
    ~FileLogSink() override;
    [[nodiscard]] bool isOpen() const;
    void write(const LogRecord& record) override;
    void flush() override;

private:
    std::unique_ptr<std::ofstream> m_file;
};

// Keeps formatted lines in memory (tests, in-game console).
class MemoryLogSink final : public LogSink {
public:
    void write(const LogRecord& record) override;
    [[nodiscard]] std::vector<std::string> lines() const;
    void clear();

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

// Thread-safe process-wide logger. Starts with one ConsoleLogSink at level Info.
namespace logging {

void setLevel(LogLevel level);
[[nodiscard]] LogLevel level();
[[nodiscard]] inline bool isEnabled(LogLevel lvl) {
    return lvl != LogLevel::Off && lvl >= level();
}

void addSink(std::shared_ptr<LogSink> sink);
void removeSink(const LogSink* sink);
void clearSinks();

void write(LogLevel lvl, std::string_view channel, std::string_view message);
void flush();

// Formats only if the level is enabled.
template <typename... Args>
void print(LogLevel lvl, std::string_view channel, std::format_string<Args...> fmt, Args&&... args) {
    if (isEnabled(lvl)) {
        write(lvl, channel, std::format(fmt, std::forward<Args>(args)...));
    }
}

} // namespace logging
} // namespace gx

#define GX_LOG_TRACE(channel, ...) ::gx::logging::print(::gx::LogLevel::Trace, channel, __VA_ARGS__)
#define GX_LOG_DEBUG(channel, ...) ::gx::logging::print(::gx::LogLevel::Debug, channel, __VA_ARGS__)
#define GX_LOG_INFO(channel, ...) ::gx::logging::print(::gx::LogLevel::Info, channel, __VA_ARGS__)
#define GX_LOG_WARN(channel, ...) ::gx::logging::print(::gx::LogLevel::Warn, channel, __VA_ARGS__)
#define GX_LOG_ERROR(channel, ...) ::gx::logging::print(::gx::LogLevel::Error, channel, __VA_ARGS__)
