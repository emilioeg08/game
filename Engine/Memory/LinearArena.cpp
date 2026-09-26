#include "Engine/Memory/LinearArena.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cstdint>

namespace gx {

LinearArena::LinearArena(usize capacityBytes)
    : m_base(static_cast<std::byte*>(::operator new(capacityBytes, std::align_val_t{kCacheLineSize}))),
      m_capacity(capacityBytes) {}

LinearArena::~LinearArena() {
    ::operator delete(m_base, std::align_val_t{kCacheLineSize});
}

void* LinearArena::tryAllocate(usize size, usize alignment) {
    GX_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0, "alignment {} is not a power of two",
              alignment);
    const auto base = reinterpret_cast<std::uintptr_t>(m_base);
    const std::uintptr_t aligned =
        (base + m_offset + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    const usize alignedOffset = static_cast<usize>(aligned - base);
    if (alignedOffset > m_capacity || size > m_capacity - alignedOffset) {
        return nullptr;
    }
    m_offset = alignedOffset + size;
    m_highWaterMark = std::max(m_highWaterMark, m_offset);
    return m_base + alignedOffset;
}

void* LinearArena::allocate(usize size, usize alignment) {
    void* memory = tryAllocate(size, alignment);
    GX_CHECK(memory != nullptr, "LinearArena exhausted: requested {} bytes, {} of {} used", size, m_offset,
             m_capacity);
    return memory;
}

void LinearArena::rewind(Marker marker) {
    GX_ASSERT(marker.offset <= m_offset, "rewinding to a marker beyond the current offset");
    m_offset = marker.offset;
}

} // namespace gx
