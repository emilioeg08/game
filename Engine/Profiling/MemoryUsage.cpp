#include "Engine/Profiling/MemoryUsage.h"

#include "Engine/Core/Platform.h"

#if GX_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h> // must follow windows.h
#elif GX_PLATFORM_LINUX
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace gx {

ProcessMemory queryProcessMemory() {
    ProcessMemory memory;
#if GX_PLATFORM_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        memory.workingSetBytes = counters.WorkingSetSize;
        memory.peakWorkingSetBytes = counters.PeakWorkingSetSize;
        memory.privateBytes = counters.PrivateUsage;
    }
#elif GX_PLATFORM_LINUX
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream fields(line);
        std::string key;
        u64 kib = 0;
        fields >> key >> kib;
        if (key == "VmRSS:") {
            memory.workingSetBytes = kib * 1024;
        } else if (key == "VmHWM:") {
            memory.peakWorkingSetBytes = kib * 1024;
        } else if (key == "VmData:") {
            memory.privateBytes = kib * 1024;
        }
    }
#endif
    return memory;
}

} // namespace gx
