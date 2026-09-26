#include "Engine/Core/Assert.h"

#include "Engine/Core/Log.h"

#include <atomic>
#include <cstdlib>

namespace gx {
namespace {

AssertAction defaultAssertHandler(const AssertInfo& info) {
    logging::write(LogLevel::Fatal, "Assert",
                   std::format("{}({}): check failed: {}{}{} (in {})", info.file, info.line, info.expression,
                               info.message[0] != '\0' ? " - " : "", info.message, info.function));
    logging::flush();
    return platform::isDebuggerAttached() ? AssertAction::Break : AssertAction::Abort;
}

std::atomic<AssertHandler> g_assertHandler{&defaultAssertHandler};

} // namespace

AssertHandler setAssertHandler(AssertHandler handler) {
    return g_assertHandler.exchange(handler != nullptr ? handler : &defaultAssertHandler);
}

namespace detail {

AssertAction handleAssertFailure(const char* expression, const char* file, int line, const char* function,
                                 const std::string& message) {
    const AssertInfo info{expression, message.c_str(), file, line, function};
    const AssertAction action = g_assertHandler.load()(info);
    if (action == AssertAction::Abort) {
        std::abort();
    }
    return action;
}

} // namespace detail
} // namespace gx
