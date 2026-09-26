#include "Engine/Time/SimTime.h"

#include <array>
#include <format>

namespace gx {
namespace {

constexpr i64 kUsPerSecond = 1'000'000;
constexpr i64 kUsPerMinute = 60 * kUsPerSecond;
constexpr i64 kUsPerHour = 60 * kUsPerMinute;
constexpr i64 kUsPerDay = 24 * kUsPerHour;
constexpr i64 kDaysPerYear = 365;
constexpr i64 kUsPerYear = kDaysPerYear * kUsPerDay;
constexpr std::array<i32, 12> kDaysInMonth = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

constexpr i64 floorDiv(i64 a, i64 b) {
    i64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

} // namespace

CalendarDate toCalendarDate(SimTime time, i64 epochYear) {
    const i64 us = time.microsecondsSinceEpoch();
    const i64 yearIndex = floorDiv(us, kUsPerYear);
    i64 rest = us - yearIndex * kUsPerYear; // [0, kUsPerYear)

    CalendarDate date;
    date.year = epochYear + yearIndex;
    const auto dayIndex = static_cast<i32>(rest / kUsPerDay);
    rest %= kUsPerDay;
    date.dayOfYear = dayIndex + 1;

    i32 remainingDays = dayIndex;
    i32 month = 0;
    while (remainingDays >= kDaysInMonth[static_cast<usize>(month)]) {
        remainingDays -= kDaysInMonth[static_cast<usize>(month)];
        ++month;
    }
    date.month = month + 1;
    date.day = remainingDays + 1;

    date.hour = static_cast<i32>(rest / kUsPerHour);
    rest %= kUsPerHour;
    date.minute = static_cast<i32>(rest / kUsPerMinute);
    rest %= kUsPerMinute;
    date.second = static_cast<i32>(rest / kUsPerSecond);
    date.microsecond = static_cast<i32>(rest % kUsPerSecond);
    return date;
}

std::string formatSimTime(SimTime time, i64 epochYear) {
    const CalendarDate d = toCalendarDate(time, epochYear);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", d.year, d.month, d.day, d.hour, d.minute,
                       d.second);
}

std::string formatDuration(SimDuration duration) {
    i64 us = duration.count();
    const char* sign = us < 0 ? "-" : "";
    if (us < 0) {
        us = -us;
    }
    if (us < kUsPerSecond) {
        return std::format("{}{:.3f}ms", sign, static_cast<f64>(us) / 1000.0);
    }
    const i64 days = us / kUsPerDay;
    const i64 hours = (us % kUsPerDay) / kUsPerHour;
    const i64 minutes = (us % kUsPerHour) / kUsPerMinute;
    const i64 seconds = (us % kUsPerMinute) / kUsPerSecond;
    if (days > 0) {
        return std::format("{}{}d {:02}:{:02}:{:02}", sign, days, hours, minutes, seconds);
    }
    return std::format("{}{:02}:{:02}:{:02}", sign, hours, minutes, seconds);
}

} // namespace gx
