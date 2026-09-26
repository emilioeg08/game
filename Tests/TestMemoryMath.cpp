#include "Tests/TestFramework.h"

#include "Engine/Math/Vec3.h"
#include "Engine/Memory/LinearArena.h"

#include <cstdint>
#include <limits>

using namespace gx;

namespace {
bool isAligned(const void* p, usize alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}
} // namespace

GX_TEST(Memory, ArenaRespectsAlignment) {
    LinearArena arena(4096);
    const void* a = arena.allocate(1, 1);
    const void* b = arena.allocate(8, 64);
    const f64* c = arena.allocateArray<f64>(10);
    const void* d = arena.allocate(3, 256);
    GX_EXPECT(a != nullptr);
    GX_EXPECT(isAligned(b, 64));
    GX_EXPECT(isAligned(c, alignof(f64)));
    GX_EXPECT(isAligned(d, 256));
}

GX_TEST(Memory, ArenaResetAndRewind) {
    LinearArena arena(1024);
    (void)arena.allocate(100, 1);
    const LinearArena::Marker marker = arena.mark();
    (void)arena.allocate(200, 1);
    GX_EXPECT_EQ(arena.used(), 300u);
    arena.rewind(marker);
    GX_EXPECT_EQ(arena.used(), 100u);
    arena.reset();
    GX_EXPECT_EQ(arena.used(), 0u);
    GX_EXPECT_EQ(arena.highWaterMark(), 300u);
}

GX_TEST(Memory, ArenaTryAllocateFailsWithoutSideEffects) {
    LinearArena arena(128);
    GX_EXPECT(arena.tryAllocate(100, 1) != nullptr);
    GX_EXPECT(arena.tryAllocate(100, 1) == nullptr);
    GX_EXPECT(arena.tryAllocate(std::numeric_limits<usize>::max(), 1) == nullptr);
    GX_EXPECT_EQ(arena.used(), 100u);
}

GX_TEST(Memory, ArenaAllocateChecksExhaustion) {
    LinearArena arena(64);
    test::ScopedAssertCapture capture;
    const void* p = arena.allocate(128, 1);
    GX_EXPECT(p == nullptr);
    GX_EXPECT_EQ(capture.count(), 1);
}

GX_TEST(Memory, ArenaCreateConstructsObjects) {
    struct Point {
        int id;
        f64 weight;
    };
    LinearArena arena(256);
    const Point* p = arena.create<Point>(Point{7, 2.5});
    GX_EXPECT_EQ(p->id, 7);
    GX_EXPECT_EQ(p->weight, 2.5);
}

GX_TEST(Math, Vec3Arithmetic) {
    constexpr Vec3d a{1.0, 2.0, 3.0};
    constexpr Vec3d b{4.0, 5.0, 6.0};
    static_assert(a + b == Vec3d{5.0, 7.0, 9.0});
    static_assert(b - a == Vec3d{3.0, 3.0, 3.0});
    static_assert(a * 2.0 == Vec3d{2.0, 4.0, 6.0});
    static_assert(2.0 * a == a * 2.0);
    static_assert(dot(a, b) == 32.0);
    static_assert(cross(Vec3d{1, 0, 0}, Vec3d{0, 1, 0}) == Vec3d{0, 0, 1});
    GX_EXPECT_EQ(length(Vec3d{3.0, 4.0, 0.0}), 5.0);

    Vec3d c = a;
    c += b;
    c -= a;
    c *= 0.5;
    GX_EXPECT(c == (Vec3d{2.0, 2.5, 3.0}));
}
