#pragma once

#include "Engine/Core/Types.h"

#include <string>

#if defined(_WIN32)
#define GX_PLATFORM_WINDOWS 1
#else
#define GX_PLATFORM_WINDOWS 0
#endif

#if defined(__linux__)
#define GX_PLATFORM_LINUX 1
#else
#define GX_PLATFORM_LINUX 0
#endif

#if defined(__APPLE__)
#define GX_PLATFORM_APPLE 1
#else
#define GX_PLATFORM_APPLE 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define GX_COMPILER_MSVC 1
#else
#define GX_COMPILER_MSVC 0
#endif

#if GX_COMPILER_MSVC
#define GX_FORCEINLINE __forceinline
#define GX_NOINLINE __declspec(noinline)
#define GX_DEBUG_BREAK() __debugbreak()
#else
#define GX_FORCEINLINE inline __attribute__((always_inline))
#define GX_NOINLINE __attribute__((noinline))
#if defined(__clang__)
#define GX_DEBUG_BREAK() __builtin_debugtrap()
#else
#define GX_DEBUG_BREAK() __builtin_trap()
#endif
#endif

#define GX_CONCAT_IMPL(a, b) a##b
#define GX_CONCAT(a, b) GX_CONCAT_IMPL(a, b)

namespace gx::platform {

// Names the calling thread for debuggers, profilers and log output.
void setCurrentThreadName(const char* name);
// Name set with setCurrentThreadName, or "thread".
[[nodiscard]] const char* currentThreadName();

[[nodiscard]] bool isDebuggerAttached();
[[nodiscard]] u32 hardwareThreadCount();
[[nodiscard]] std::string cpuBrandString();

// Monotonic wall clock in nanoseconds. For measurement only: never feed it into simulation state.
[[nodiscard]] u64 monotonicNanoseconds();

// Hint for spin-wait loops (x86 PAUSE / ARM YIELD).
void cpuRelax();

} // namespace gx::platform
