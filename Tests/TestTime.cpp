#include "Tests/TestFramework.h"

#include "Engine/Time/SimClock.h"
#include "Engine/Time/SimTime.h"
#include "Engine/Time/TimeController.h"

using namespace gx;

GX_TEST(Time, DurationConversions) {
    static_assert(SimDuration::hours(1) == SimDuration::minutes(60));
    static_assert(SimDuration::days(1).count() == 86'400'000'000);
    static_assert(SimDuration::years(1) == SimDuration::days(365));
    static_assert(SimDuration::hours(3) / SimDuration::minutes(30) == 6);
    GX_EXPECT_EQ(SimDuration::milliseconds(1500).toSeconds(), 1.5);
    GX_EXPECT_EQ(SimDuration::minutes(30).toHours(), 0.5);
    GX_EXPECT_EQ(SimDuration::hours(36).toDays(), 1.5);
}

GX_TEST(Time, TimePointArithmetic) {
    constexpr SimTime start = SimTime::epoch();
    constexpr SimTime later = start + SimDuration::hours(2);
    static_assert(later - start == SimDuration::hours(2));
    static_assert(later > start);
    static_assert(later - SimDuration::hours(2) == start);
    SimTime t = start;
    t += SimDuration::seconds(5);
    GX_EXPECT(t == start + SimDuration::seconds(5));
}

GX_TEST(Time, CalendarUsesMonths) {
    GX_EXPECT_EQ(formatSimTime(SimTime::epoch()), std::string("0001-01-01 00:00:00"));
    const SimTime t = SimTime::epoch() + SimDuration::years(2) + SimDuration::days(59) +
                      SimDuration::hours(13) + SimDuration::minutes(5) + SimDuration::seconds(7);
    const CalendarDate date = toCalendarDate(t);
    GX_EXPECT_EQ(date.year, 3);
    GX_EXPECT_EQ(date.month, 3); // day index 59 = March 1st (no leap years)
    GX_EXPECT_EQ(date.day, 1);
    GX_EXPECT_EQ(date.dayOfYear, 60);
    GX_EXPECT_EQ(formatSimTime(t, 2350), std::string("2352-03-01 13:05:07"));
    GX_EXPECT_EQ(formatSimTime(SimTime::epoch() + SimDuration::days(364)),
                 std::string("0001-12-31 00:00:00"));
    GX_EXPECT_EQ(formatSimTime(SimTime::epoch() - SimDuration::days(1)), std::string("0000-12-31 00:00:00"));
}

GX_TEST(Time, FormatDuration) {
    GX_EXPECT_EQ(formatDuration(SimDuration::milliseconds(12)), std::string("12.000ms"));
    GX_EXPECT_EQ(formatDuration(SimDuration::hours(4) + SimDuration::seconds(6)), std::string("04:00:06"));
    GX_EXPECT_EQ(formatDuration(SimDuration::days(3) + SimDuration::minutes(5)), std::string("3d 00:05:00"));
}

GX_TEST(Time, ClockRejectsGoingBackwards) {
    SimClock clock;
    clock.advanceTo(SimTime::epoch() + SimDuration::seconds(10));
    GX_EXPECT(clock.elapsed() == SimDuration::seconds(10));
    test::ScopedAssertCapture capture;
    clock.advanceTo(SimTime::epoch() + SimDuration::seconds(5));
    GX_EXPECT_EQ(capture.count(), 1);
}

GX_TEST(Time, ControllerAcceleratesTime) {
    TimeController controller(SimTime::epoch());
    controller.setSpeed(60);
    const SimTime target = controller.update(100'000'000, SimTime::epoch()); // 100 ms real
    GX_EXPECT(target == SimTime::epoch() + SimDuration::seconds(6));
}

GX_TEST(Time, ControllerPauseFreezesTime) {
    TimeController controller(SimTime::epoch());
    controller.setSpeed(10);
    controller.setPaused(true);
    GX_EXPECT(controller.update(100'000'000, SimTime::epoch()) == SimTime::epoch());
    controller.setPaused(false);
    GX_EXPECT(controller.update(100'000'000, SimTime::epoch()) == SimTime::epoch() + SimDuration::seconds(1));
}

GX_TEST(Time, ControllerCarriesSubMicrosecondRemainders) {
    TimeController controller(SimTime::epoch());
    GX_EXPECT(controller.update(1'500, SimTime::epoch()) == SimTime::fromMicroseconds(1));
    GX_EXPECT(controller.update(500, SimTime::epoch()) == SimTime::fromMicroseconds(2));
}

GX_TEST(Time, ControllerDropsLagBeyondLimit) {
    TimeController controller(SimTime::epoch(), TimeController::Config{.maxRealLagNs = 250'000'000});
    controller.setSpeed(1000);
    // One real second at 1000x asks for 1000 s, but only 250 ms of real lag (250 s) may accumulate.
    const SimTime target = controller.update(1'000'000'000, SimTime::epoch());
    GX_EXPECT(target == SimTime::epoch() + SimDuration::seconds(250));
    GX_EXPECT(controller.droppedTime() == SimDuration::seconds(750));
}
