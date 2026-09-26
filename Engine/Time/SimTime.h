#pragma once

#include "Engine/Core/Types.h"

#include <compare>
#include <string>

namespace gx {

// Simulation time is integral: microseconds in a signed 64-bit counter (range +-292,000 years).
// Integer time never drifts over long campaigns and is bit-identical on every platform; convert to
// floating point only at the edge (e.g. a physics step's dt).
class SimDuration {
public:
    constexpr SimDuration() = default;

    static constexpr SimDuration microseconds(i64 value) { return SimDuration(value); }
    static constexpr SimDuration milliseconds(i64 value) { return SimDuration(value * 1'000); }
    static constexpr SimDuration seconds(i64 value) { return SimDuration(value * 1'000'000); }
    static constexpr SimDuration minutes(i64 value) { return seconds(value * 60); }
    static constexpr SimDuration hours(i64 value) { return seconds(value * 3'600); }
    static constexpr SimDuration days(i64 value) { return seconds(value * 86'400); }
    // Calendar placeholder until game.md defines one: 365-day years, no leap years.
    static constexpr SimDuration years(i64 value) { return days(value * 365); }

    // Microseconds.
    [[nodiscard]] constexpr i64 count() const { return m_us; }
    [[nodiscard]] constexpr f64 toSeconds() const { return static_cast<f64>(m_us) / 1'000'000.0; }
    [[nodiscard]] constexpr f64 toHours() const { return static_cast<f64>(m_us) / 3'600'000'000.0; }
    [[nodiscard]] constexpr f64 toDays() const { return static_cast<f64>(m_us) / 86'400'000'000.0; }

    friend constexpr SimDuration operator+(SimDuration a, SimDuration b) {
        return SimDuration(a.m_us + b.m_us);
    }
    friend constexpr SimDuration operator-(SimDuration a, SimDuration b) {
        return SimDuration(a.m_us - b.m_us);
    }
    friend constexpr SimDuration operator*(SimDuration d, i64 factor) { return SimDuration(d.m_us * factor); }
    friend constexpr SimDuration operator*(i64 factor, SimDuration d) { return SimDuration(d.m_us * factor); }
    friend constexpr SimDuration operator/(SimDuration d, i64 divisor) {
        return SimDuration(d.m_us / divisor);
    }
    friend constexpr i64 operator/(SimDuration a, SimDuration b) { return a.m_us / b.m_us; }
    friend constexpr SimDuration operator%(SimDuration a, SimDuration b) {
        return SimDuration(a.m_us % b.m_us);
    }
    constexpr SimDuration& operator+=(SimDuration d) {
        m_us += d.m_us;
        return *this;
    }
    constexpr SimDuration& operator-=(SimDuration d) {
        m_us -= d.m_us;
        return *this;
    }
    constexpr auto operator<=>(const SimDuration&) const = default;

private:
    constexpr explicit SimDuration(i64 us) : m_us(us) {}
    i64 m_us = 0;
};

// A point in simulation time: microseconds since the campaign epoch.
class SimTime {
public:
    constexpr SimTime() = default;

    static constexpr SimTime epoch() { return {}; }
    static constexpr SimTime fromMicroseconds(i64 us) {
        SimTime time;
        time.m_us = us;
        return time;
    }

    [[nodiscard]] constexpr i64 microsecondsSinceEpoch() const { return m_us; }
    [[nodiscard]] constexpr SimDuration sinceEpoch() const { return SimDuration::microseconds(m_us); }

    friend constexpr SimTime operator+(SimTime t, SimDuration d) {
        return fromMicroseconds(t.m_us + d.count());
    }
    friend constexpr SimTime operator-(SimTime t, SimDuration d) {
        return fromMicroseconds(t.m_us - d.count());
    }
    friend constexpr SimDuration operator-(SimTime a, SimTime b) {
        return SimDuration::microseconds(a.m_us - b.m_us);
    }
    constexpr SimTime& operator+=(SimDuration d) {
        m_us += d.count();
        return *this;
    }
    constexpr auto operator<=>(const SimTime&) const = default;

private:
    i64 m_us = 0;
};

// Placeholder calendar until game.md defines one: 12 months with Gregorian month lengths, 365-day years
// (no leap years, so every year has the same length in simulation time).
struct CalendarDate {
    i64 year = 1;
    i32 month = 1;     // 1..12
    i32 day = 1;       // 1..31
    i32 dayOfYear = 1; // 1..365
    i32 hour = 0;
    i32 minute = 0;
    i32 second = 0;
    i32 microsecond = 0;
};

// `epochYear` is the calendar year at SimTime::epoch().
[[nodiscard]] CalendarDate toCalendarDate(SimTime time, i64 epochYear = 1);
// "YYYY-MM-DD hh:mm:ss"
[[nodiscard]] std::string formatSimTime(SimTime time, i64 epochYear = 1);
// "3d 04:05:06", "04:05:06" or "12.500ms"
[[nodiscard]] std::string formatDuration(SimDuration duration);

} // namespace gx
