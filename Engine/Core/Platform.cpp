#include "Engine/Core/Platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#if GX_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h>
#include <windows.h>
#else
#include <fstream>
#include <pthread.h>
#endif

#if !GX_PLATFORM_WINDOWS && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace gx::platform {

namespace {
constexpr usize kMaxThreadNameLength = 31;
thread_local char t_threadName[kMaxThreadNameLength + 1] = "thread";
} // namespace

void setCurrentThreadName(const char* name) {
    usize length = 0;
    while (name[length] != '\0' && length < kMaxThreadNameLength) {
        t_threadName[length] = name[length];
        ++length;
    }
    t_threadName[length] = '\0';

#if GX_PLATFORM_WINDOWS
    wchar_t wide[kMaxThreadNameLength + 1] = {};
    for (usize i = 0; i <= length; ++i) {
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(t_threadName[i])); // thread names are ASCII
    }
    SetThreadDescription(GetCurrentThread(), wide);
#elif GX_PLATFORM_LINUX
    char shortName[16] = {}; // Linux limit: 15 characters plus terminator
    for (usize i = 0; i < 15 && t_threadName[i] != '\0'; ++i) {
        shortName[i] = t_threadName[i];
    }
    pthread_setname_np(pthread_self(), shortName);
#elif GX_PLATFORM_APPLE
    pthread_setname_np(t_threadName);
#endif
}

const char* currentThreadName() {
    return t_threadName;
}

bool isDebuggerAttached() {
#if GX_PLATFORM_WINDOWS
    return IsDebuggerPresent() != 0;
#elif GX_PLATFORM_LINUX
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("TracerPid:", 0) == 0) {
            return line.find_first_not_of(" \t0", 10) != std::string::npos;
        }
    }
    return false;
#else
    return false;
#endif
}

u32 hardwareThreadCount() {
    return std::max(1u, std::thread::hardware_concurrency());
}

std::string cpuBrandString() {
    std::array<char, 49> brand{};
#if GX_PLATFORM_WINDOWS && (defined(_M_X64) || defined(_M_IX86))
    std::array<int, 4> regs{};
    __cpuid(regs.data(), static_cast<int>(0x80000000u));
    if (static_cast<u32>(regs[0]) >= 0x80000004u) {
        for (u32 i = 0; i < 3; ++i) {
            __cpuid(regs.data(), static_cast<int>(0x80000002u + i));
            std::memcpy(brand.data() + i * 16, regs.data(), 16);
        }
    }
#elif defined(__x86_64__) || defined(__i386__)
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u) {
        for (u32 i = 0; i < 3; ++i) {
            unsigned regs[4] = {};
            __get_cpuid(0x80000002u + i, &regs[0], &regs[1], &regs[2], &regs[3]);
            std::memcpy(brand.data() + i * 16, regs, 16);
        }
    }
#endif
    std::string result(brand.data());
    const auto first = result.find_first_not_of(' ');
    const auto last = result.find_last_not_of(' ');
    return first == std::string::npos ? std::string("unknown") : result.substr(first, last - first + 1);
}

u64 monotonicNanoseconds() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

void cpuRelax() {
#if GX_PLATFORM_WINDOWS && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif GX_PLATFORM_WINDOWS && defined(_M_ARM64)
    __yield();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

} // namespace gx::platform
