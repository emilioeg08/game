#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Types.h"

#include <bit>

namespace gx {

// SplitMix64 (Steele, Lea & Flood 2014). Used to expand seeds into generator state.
constexpr u64 splitMix64(u64& state) {
    state += 0x9e3779b97f4a7c15ull;
    return mix64(state);
}

// xoshiro256** 1.0 (Blackman & Vigna). Integer-only, so every seeded sequence is bit-identical on every
// platform and compiler.
//
// Not thread-safe, and deliberately never shared between jobs: derive an independent generator per unit of
// work with forStream(seed, entity, tick) so results do not depend on thread count or execution order.
class Rng {
public:
    explicit constexpr Rng(u64 seed) {
        u64 expander = seed;
        for (u64& word : m_state) {
            word = splitMix64(expander);
        }
    }

    // Reproducible, independent stream for a (seed, a, b) key, e.g. (worldSeed, entityId, runIndex).
    [[nodiscard]] static constexpr Rng forStream(u64 seed, u64 a, u64 b = 0) {
        return Rng(hashCombine(hashCombine(seed, a), b));
    }

    constexpr u64 nextU64() {
        const u64 result = std::rotl(m_state[1] * 5, 7) * 9;
        const u64 t = m_state[1] << 17;
        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];
        m_state[2] ^= t;
        m_state[3] = std::rotl(m_state[3], 45);
        return result;
    }

    constexpr u32 nextU32() { return static_cast<u32>(nextU64() >> 32); }

    // Uniform integer in [0, bound). Lemire's multiply-shift with rejection: unbiased.
    constexpr u32 uniformU32(u32 bound) {
        GX_ASSERT(bound > 0, "uniformU32 needs a positive bound");
        u64 product = static_cast<u64>(nextU32()) * bound;
        u32 low = static_cast<u32>(product);
        if (low < bound) {
            const u32 threshold = (0u - bound) % bound;
            while (low < threshold) {
                product = static_cast<u64>(nextU32()) * bound;
                low = static_cast<u32>(product);
            }
        }
        return static_cast<u32>(product >> 32);
    }

    // Uniform integer in [lo, hi], both inclusive.
    constexpr i32 rangeI32(i32 lo, i32 hi) {
        GX_ASSERT(lo <= hi, "rangeI32 needs lo <= hi");
        const u32 span = static_cast<u32>(static_cast<i64>(hi) - static_cast<i64>(lo) + 1);
        const u32 offset = span == 0 ? nextU32() : uniformU32(span); // span == 0: the full 32-bit range
        return static_cast<i32>(static_cast<u32>(lo) + offset);
    }

    // Uniform double in [0, 1) from the top 53 bits: exact and platform-independent.
    constexpr f64 nextF64() { return static_cast<f64>(nextU64() >> 11) * 0x1.0p-53; }

    constexpr f64 uniform(f64 lo, f64 hi) { return lo + (hi - lo) * nextF64(); }

    constexpr bool chance(f64 probability) { return nextF64() < probability; }

private:
    u64 m_state[4]{};
};

} // namespace gx
