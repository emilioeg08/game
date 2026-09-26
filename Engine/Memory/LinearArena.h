#pragma once

#include "Engine/Core/Types.h"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace gx {

// Bump allocator over one fixed block. Allocating is a pointer increment; memory is released all at once
// with reset() (e.g. every simulation step) or back to a marker with rewind().
//
// Destructors are never run, so only trivially destructible types may live here. Not thread-safe: use one
// arena per thread.
class LinearArena {
public:
    struct Marker {
        usize offset = 0;
    };

    explicit LinearArena(usize capacityBytes);
    ~LinearArena();
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;

    // nullptr if the request does not fit. `alignment` must be a power of two.
    [[nodiscard]] void* tryAllocate(usize size, usize alignment = alignof(std::max_align_t));
    // Checked: running out of arena space is a sizing bug, so it fails a GX_CHECK.
    [[nodiscard]] void* allocate(usize size, usize alignment = alignof(std::max_align_t));

    // Uninitialized storage for `count` objects.
    template <typename T>
    [[nodiscard]] T* allocateArray(usize count) {
        static_assert(std::is_trivially_destructible_v<T> && std::is_trivially_default_constructible_v<T>,
                      "LinearArena arrays must be trivial types");
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "LinearArena never runs destructors");
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    void reset() { m_offset = 0; }
    [[nodiscard]] Marker mark() const { return {m_offset}; }
    void rewind(Marker marker);

    [[nodiscard]] usize used() const { return m_offset; }
    [[nodiscard]] usize capacity() const { return m_capacity; }
    [[nodiscard]] usize highWaterMark() const { return m_highWaterMark; }

private:
    std::byte* m_base = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
    usize m_highWaterMark = 0;
};

} // namespace gx
