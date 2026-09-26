#pragma once

// Minimal in-house test framework: registration, expectations, a runner with suite/filter selection.
// Kept dependency-free on purpose; replace with a full framework if the needs outgrow it.

#include "Engine/Core/Assert.h"

#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gx::test {

using TestFunction = void (*)();

struct Registrar {
    Registrar(const char* suite, const char* name, TestFunction function, const char* file, int line);
};

// Thrown by GX_REQUIRE to abort the current test.
struct RequireFailed {};

// Thread-safe: expectations may fail inside jobs running on worker threads.
void reportFailure(const char* file, int line, const std::string& message);

template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::format("{}", value);
    } else if constexpr (std::is_enum_v<T>) {
        return std::format("{}", static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return std::format("\"{}\"", std::string_view(value));
    } else {
        return "<value>";
    }
}

template <typename T>
inline constexpr bool kIsPlainInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
    !std::is_same_v<T, char32_t>;

// Mixed signed/unsigned integers compare by value (no sign-conversion surprises or warnings).
template <typename A, typename B>
bool equals(const A& a, const B& b) {
    if constexpr (kIsPlainInteger<A> && kIsPlainInteger<B>) {
        return std::cmp_equal(a, b);
    } else {
        return a == b;
    }
}

// Captures GX_CHECK/GX_ASSERT failures instead of aborting, for tests that verify an invariant fires.
// Main thread only.
class ScopedAssertCapture {
public:
    ScopedAssertCapture() : m_previous(setAssertHandler(&capture)) {
        s_count = 0;
        s_lastExpression.clear();
        s_lastMessage.clear();
    }
    ~ScopedAssertCapture() { setAssertHandler(m_previous); }
    ScopedAssertCapture(const ScopedAssertCapture&) = delete;
    ScopedAssertCapture& operator=(const ScopedAssertCapture&) = delete;

    [[nodiscard]] int count() const { return s_count; }
    [[nodiscard]] const std::string& lastExpression() const { return s_lastExpression; }
    [[nodiscard]] const std::string& lastMessage() const { return s_lastMessage; }

private:
    static AssertAction capture(const AssertInfo& info) {
        ++s_count;
        s_lastExpression = info.expression;
        s_lastMessage = info.message;
        return AssertAction::Continue;
    }

    AssertHandler m_previous;
    static inline int s_count = 0;
    static inline std::string s_lastExpression;
    static inline std::string s_lastMessage;
};

} // namespace gx::test

#define GX_TEST(suite, name)                                                                                 \
    static void gxTest_##suite##_##name();                                                                   \
    static const ::gx::test::Registrar gxTestRegistrar_##suite##_##name(                                     \
        #suite, #name, &gxTest_##suite##_##name, __FILE__, __LINE__);                                        \
    static void gxTest_##suite##_##name()

#define GX_EXPECT(condition)                                                                                 \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            ::gx::test::reportFailure(__FILE__, __LINE__, "expected: " #condition);                          \
        }                                                                                                    \
    } while (false)

#define GX_EXPECT_EQ(actual, expected)                                                                       \
    do {                                                                                                     \
        const auto& gxActual = (actual);                                                                     \
        const auto& gxExpected = (expected);                                                                 \
        if (!::gx::test::equals(gxActual, gxExpected)) {                                                     \
            ::gx::test::reportFailure(                                                                       \
                __FILE__, __LINE__,                                                                          \
                std::format("expected {} == {}\n        actual:   {}\n        expected: {}", #actual,        \
                            #expected, ::gx::test::describe(gxActual), ::gx::test::describe(gxExpected)));   \
        }                                                                                                    \
    } while (false)

#define GX_EXPECT_NEAR(actual, expected, tolerance)                                                          \
    do {                                                                                                     \
        const double gxActual = static_cast<double>(actual);                                                 \
        const double gxExpected = static_cast<double>(expected);                                             \
        if (!(std::abs(gxActual - gxExpected) <= static_cast<double>(tolerance))) {                          \
            ::gx::test::reportFailure(__FILE__, __LINE__,                                                    \
                                      std::format("expected {} ~= {} (+-{}): {} vs {}", #actual, #expected,  \
                                                  static_cast<double>(tolerance), gxActual, gxExpected));    \
        }                                                                                                    \
    } while (false)

#define GX_REQUIRE(condition)                                                                                \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            ::gx::test::reportFailure(__FILE__, __LINE__, "required: " #condition);                          \
            throw ::gx::test::RequireFailed{};                                                               \
        }                                                                                                    \
    } while (false)
