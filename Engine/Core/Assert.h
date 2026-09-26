#pragma once

#include "Engine/Core/Platform.h"

#include <format>
#include <string>
#include <utility>

#ifndef GX_ENABLE_ASSERTS
#define GX_ENABLE_ASSERTS 1
#endif

namespace gx {

struct AssertInfo {
    const char* expression;
    const char* message; // empty when the check has no message
    const char* file;
    int line;
    const char* function;
};

enum class AssertAction : u8 {
    Abort,    // terminate the process (default without a debugger)
    Break,    // debugger breakpoint, then continue (default with a debugger)
    Continue, // ignore the failure (tests that verify a check fires)
};

using AssertHandler = AssertAction (*)(const AssertInfo& info);

// Installs a process-wide handler and returns the previous one. nullptr restores the default handler,
// which logs the failure and aborts (or breaks when a debugger is attached).
AssertHandler setAssertHandler(AssertHandler handler);

namespace detail {

AssertAction handleAssertFailure(const char* expression, const char* file, int line, const char* function,
                                 const std::string& message);

inline std::string assertMessage() {
    return {};
}

template <typename... Args>
std::string assertMessage(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}

} // namespace detail
} // namespace gx

// Always-on invariant check, in every build configuration. Use where a violation would corrupt simulation
// state or make results meaningless. Optional message: GX_CHECK(x > 0, "x was {}", x).
#define GX_CHECK(condition, ...)                                                                             \
    do {                                                                                                     \
        if (!(condition)) [[unlikely]] {                                                                     \
            if (::gx::detail::handleAssertFailure(#condition, __FILE__, __LINE__, __func__,                  \
                                                  ::gx::detail::assertMessage(__VA_ARGS__)) ==               \
                ::gx::AssertAction::Break) {                                                                 \
                GX_DEBUG_BREAK();                                                                            \
            }                                                                                                \
        }                                                                                                    \
    } while (false)

// Development check: compiled out unless GX_ENABLE_ASSERTS (Debug and Profile presets). Use on hot paths.
#if GX_ENABLE_ASSERTS
#define GX_ASSERT(condition, ...) GX_CHECK(condition, __VA_ARGS__)
#else
#define GX_ASSERT(condition, ...)                                                                            \
    do {                                                                                                     \
        (void)sizeof(!(condition));                                                                          \
    } while (false)
#endif
