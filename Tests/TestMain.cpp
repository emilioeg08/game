#include "Tests/TestFramework.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Time/Stopwatch.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace gx::test {
namespace {

struct TestCase {
    const char* suite;
    const char* name;
    TestFunction function;
    const char* file;
    int line;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

std::mutex g_failureMutex;
int g_failuresInCurrentTest = 0;

} // namespace

Registrar::Registrar(const char* suite, const char* name, TestFunction function, const char* file, int line) {
    registry().push_back({suite, name, function, file, line});
}

void reportFailure(const char* file, int line, const std::string& message) {
    std::lock_guard lock(g_failureMutex);
    ++g_failuresInCurrentTest;
    std::printf("    %s(%d): %s\n", file, line, message.c_str());
    std::fflush(stdout);
}

} // namespace gx::test

namespace {

void printUsage() {
    std::printf("Usage: gx_tests [--suite <name>] [--filter <substring>] [--list]\n");
}

} // namespace

int main(int argc, char** argv) {
    using namespace gx;
    using namespace gx::test;

    std::string suite;
    std::string filter;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--suite" && i + 1 < argc) {
            suite = argv[++i];
        } else if (arg == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg == "--list") {
            listOnly = true;
        } else {
            printUsage();
            return 2;
        }
    }

    platform::setCurrentThreadName("main");
    logging::setLevel(LogLevel::Warn); // keep test output readable; failures print on their own

    std::vector<TestCase> selected;
    for (const TestCase& test : registry()) {
        const std::string fullName = std::string(test.suite) + "." + test.name;
        if ((suite.empty() || suite == test.suite) &&
            (filter.empty() || fullName.find(filter) != std::string::npos)) {
            selected.push_back(test);
        }
    }
    // Registration order across translation units is unspecified: order by suite, keep file order inside it.
    std::stable_sort(selected.begin(), selected.end(), [](const TestCase& a, const TestCase& b) {
        return std::string_view(a.suite) < b.suite;
    });

    if (selected.empty()) {
        std::printf("no tests match (suite '%s', filter '%s')\n", suite.c_str(), filter.c_str());
        return 2;
    }
    if (listOnly) {
        for (const TestCase& test : selected) {
            std::printf("%s.%s\n", test.suite, test.name);
        }
        return 0;
    }

    int failedTests = 0;
    const Stopwatch total;
    for (const TestCase& test : selected) {
        {
            std::lock_guard lock(g_failureMutex);
            g_failuresInCurrentTest = 0;
        }
        std::printf("[ RUN  ] %s.%s\n", test.suite, test.name);
        std::fflush(stdout);
        const Stopwatch timer;
        try {
            test.function();
        } catch (const RequireFailed&) {
            // already reported
        } catch (const std::exception& e) {
            reportFailure(test.file, test.line, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            reportFailure(test.file, test.line, "unexpected non-standard exception");
        }
        int failures = 0;
        {
            std::lock_guard lock(g_failureMutex);
            failures = g_failuresInCurrentTest;
        }
        std::printf("%s %s.%s (%.1f ms)\n", failures == 0 ? "[  OK  ]" : "[ FAIL ]", test.suite, test.name,
                    timer.elapsedMs());
        if (failures != 0) {
            ++failedTests;
        }
    }

    std::printf("\n%zu tests, %zu passed, %d failed (%.1f ms)\n", selected.size(),
                selected.size() - static_cast<usize>(failedTests), failedTests, total.elapsedMs());
    return failedTests == 0 ? 0 : 1;
}
