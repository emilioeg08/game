#include "Tests/TestFramework.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"

#include <array>
#include <memory>

using namespace gx;

GX_TEST(Core, SplitMix64MatchesReferenceSequence) {
    // Published reference outputs of SplitMix64 for seed 0.
    u64 state = 0;
    GX_EXPECT_EQ(splitMix64(state), 0xe220a8397b1dcdafull);
    GX_EXPECT_EQ(splitMix64(state), 0x6e789e6aa1b965f4ull);
    GX_EXPECT_EQ(splitMix64(state), 0x06c45d188009454full);
}

GX_TEST(Core, RngMatchesReferenceXoshiro256StarStar) {
    // xoshiro256** seeded with SplitMix64(42), values from an independent reference implementation.
    // Guards bit-exact reproducibility of every seeded sequence across compilers and platforms.
    Rng rng(42);
    GX_EXPECT_EQ(rng.nextU64(), 0x15780b2e0c2ec716ull);
    GX_EXPECT_EQ(rng.nextU64(), 0x6104d9866d113a7eull);
    GX_EXPECT_EQ(rng.nextU64(), 0xae17533239e499a1ull);
    GX_EXPECT_EQ(rng.nextU64(), 0xecb8ad4703b360a1ull);

    static_assert(
        [] {
            Rng compileTime(42);
            return compileTime.nextU64();
        }() == 0x15780b2e0c2ec716ull,
        "Rng must be usable in constant expressions");
}

GX_TEST(Core, RngStreamsAreReproducibleAndIndependent) {
    Rng a = Rng::forStream(7, 100, 3);
    Rng b = Rng::forStream(7, 100, 3);
    Rng otherEntity = Rng::forStream(7, 101, 3);
    Rng otherTick = Rng::forStream(7, 100, 4);
    Rng otherSeed = Rng::forStream(8, 100, 3);
    const u64 first = a.nextU64();
    GX_EXPECT_EQ(first, b.nextU64());
    GX_EXPECT(first != otherEntity.nextU64());
    GX_EXPECT(first != otherTick.nextU64());
    GX_EXPECT(first != otherSeed.nextU64());
}

GX_TEST(Core, RngUniformU32IsInRangeAndBalanced) {
    Rng rng(123);
    std::array<u32, 10> buckets{};
    constexpr u32 kSamples = 200'000;
    for (u32 i = 0; i < kSamples; ++i) {
        const u32 v = rng.uniformU32(10);
        GX_REQUIRE(v < 10);
        ++buckets[v];
    }
    for (const u32 count : buckets) {
        GX_EXPECT(count > kSamples / 10 * 95 / 100 && count < kSamples / 10 * 105 / 100);
    }
}

GX_TEST(Core, RngRangeI32IsInclusive) {
    Rng rng(5);
    bool sawLow = false;
    bool sawHigh = false;
    for (int i = 0; i < 10'000; ++i) {
        const i32 v = rng.rangeI32(-3, 3);
        GX_REQUIRE(v >= -3 && v <= 3);
        sawLow = sawLow || v == -3;
        sawHigh = sawHigh || v == 3;
    }
    GX_EXPECT(sawLow && sawHigh);
}

GX_TEST(Core, RngF64IsInUnitInterval) {
    Rng rng(77);
    f64 sum = 0.0;
    constexpr int kSamples = 100'000;
    for (int i = 0; i < kSamples; ++i) {
        const f64 v = rng.nextF64();
        GX_REQUIRE(v >= 0.0 && v < 1.0);
        sum += v;
    }
    GX_EXPECT_NEAR(sum / kSamples, 0.5, 0.01);
}

GX_TEST(Core, Fnv1aMatchesReferenceVectors) {
    static_assert(fnv1a64("") == 0xcbf29ce484222325ull);
    GX_EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    GX_EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ull);
}

GX_TEST(Core, StateHasherIsOrderAndBitSensitive) {
    StateHasher forward;
    forward.add(1.0);
    forward.add(2.0);
    StateHasher reversed;
    reversed.add(2.0);
    reversed.add(1.0);
    GX_EXPECT(forward.value() != reversed.value());

    StateHasher positiveZero;
    positiveZero.add(0.0);
    StateHasher negativeZero;
    negativeZero.add(-0.0);
    GX_EXPECT(positiveZero.value() != negativeZero.value());

    StateHasher a;
    a.add(u64{5});
    StateHasher b;
    b.add(u64{5});
    GX_EXPECT_EQ(a.value(), b.value());
}

GX_TEST(Core, CheckInvokesInstalledAssertHandler) {
    test::ScopedAssertCapture capture;
    int value = 3;
    GX_CHECK(value == 4, "value was {}", value);
    GX_CHECK(value == 3);
    GX_EXPECT_EQ(capture.count(), 1);
    GX_EXPECT_EQ(capture.lastExpression(), std::string("value == 4"));
    GX_EXPECT_EQ(capture.lastMessage(), std::string("value was 3"));
}

GX_TEST(Core, AssertFollowsBuildConfiguration) {
    test::ScopedAssertCapture capture;
    int value = 1;
    GX_ASSERT(value == 2);
    GX_EXPECT_EQ(capture.count(), GX_ENABLE_ASSERTS ? 1 : 0);
}

GX_TEST(Core, LogRoutesToSinksAndFiltersByLevel) {
    auto sink = std::make_shared<MemoryLogSink>();
    const LogLevel previous = logging::level();
    logging::clearSinks(); // keep the expected output off the console
    logging::addSink(sink);
    logging::setLevel(LogLevel::Info);
    GX_LOG_DEBUG("LogTest", "filtered out {}", 1);
    GX_LOG_INFO("LogTest", "visible {}", 2);
    GX_LOG_ERROR("LogTest", "error {}", 3);
    logging::clearSinks();
    logging::addSink(std::make_shared<ConsoleLogSink>());
    logging::setLevel(previous);

    const auto lines = sink->lines();
    GX_REQUIRE(lines.size() == 2);
    GX_EXPECT(lines[0].find("[LogTest] visible 2") != std::string::npos);
    GX_EXPECT(lines[1].find("[ERROR]") != std::string::npos);
    GX_EXPECT(lines[1].find("[main]") != std::string::npos);
}

GX_TEST(Core, ParseLogLevel) {
    LogLevel level = LogLevel::Info;
    GX_EXPECT(parseLogLevel("warn", level));
    GX_EXPECT(level == LogLevel::Warn);
    GX_EXPECT(parseLogLevel("TRACE", level));
    GX_EXPECT(level == LogLevel::Trace);
    GX_EXPECT(!parseLogLevel("verbose", level));
}
