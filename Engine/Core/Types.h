#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace gx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

inline constexpr usize kCacheLineSize = 64;

// Deterministic simulation relies on IEEE-754 binary64 arithmetic.
static_assert(std::numeric_limits<f64>::is_iec559 && sizeof(f64) == 8,
              "IEEE-754 double precision is required");

} // namespace gx
