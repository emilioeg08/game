#pragma once

#include "Engine/Core/Types.h"

#include <bit>
#include <string_view>
#include <type_traits>

namespace gx {

inline constexpr u64 kFnv1aOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr u64 kFnv1aPrime = 0x100000001b3ull;

// FNV-1a 64-bit. Stable across platforms; use for identifiers derived from text (names, content keys).
constexpr u64 fnv1a64(std::string_view text, u64 hash = kFnv1aOffsetBasis) {
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= kFnv1aPrime;
    }
    return hash;
}

// SplitMix64 finalizer: fast bijective mix with full avalanche.
constexpr u64 mix64(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

constexpr u64 hashCombine(u64 seed, u64 value) {
    return mix64(seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
}

// Order-sensitive checksum of simulation state for determinism checks. Values are hashed by bit pattern,
// so any bit-level divergence is detected (including -0.0 vs +0.0). Not a cryptographic hash.
class StateHasher {
public:
    constexpr void addU64(u64 value) { m_hash = (m_hash ^ mix64(value)) * kFnv1aPrime; }

    template <typename T>
        requires(std::is_integral_v<T> || std::is_enum_v<T>)
    constexpr void add(T value) {
        if constexpr (std::is_enum_v<T>) {
            addU64(static_cast<u64>(static_cast<std::underlying_type_t<T>>(value)));
        } else {
            addU64(static_cast<u64>(value));
        }
    }

    constexpr void add(f64 value) { addU64(std::bit_cast<u64>(value)); }

    [[nodiscard]] constexpr u64 value() const { return mix64(m_hash); }

private:
    u64 m_hash = kFnv1aOffsetBasis;
};

} // namespace gx
