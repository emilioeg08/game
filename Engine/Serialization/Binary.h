#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"

#include <bit>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Binary serialization with an explicit little-endian layout, identical on every platform.
//
// BinaryWriter and BinaryReader share one `io(value)` interface, so a type describes its layout once:
//
//     struct Cargo {
//         u32 good = 0;
//         f64 amount = 0.0;
//         template <typename Archive> void io(Archive& ar) { ar.io(good); ar.io(amount); }
//     };
//
// Supported directly: bool, integers, enums, f32/f64, std::string, Vec3d, SimTime, SimDuration,
// std::vector of any supported type, and any type with an `io` member template.
//
// Chunks (tag + version + byte size) group data so readers can validate structure and skip fields
// appended by newer writers.
namespace gx {

// Four-character chunk tag, e.g. fourCC("WRLD").
constexpr u32 fourCC(const char (&text)[5]) {
    return static_cast<u32>(static_cast<u8>(text[0])) | (static_cast<u32>(static_cast<u8>(text[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(text[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(text[3])) << 24);
}

[[nodiscard]] std::string fourCCToString(u32 tag);

namespace detail {
template <typename T>
struct IsStdVector : std::false_type {};
template <typename T, typename Allocator>
struct IsStdVector<std::vector<T, Allocator>> : std::true_type {};
} // namespace detail

template <typename T, typename Archive>
concept HasIoMember = requires(T& value, Archive& archive) { value.io(archive); };

class BinaryWriter {
public:
    static constexpr bool kIsReading = false;

    struct ChunkMark {
        usize sizeOffset = 0;
    };

    void writeU8(u8 value) { m_bytes.push_back(static_cast<std::byte>(value)); }
    void writeU16(u16 value) { writeLittleEndian(value, 2); }
    void writeU32(u32 value) { writeLittleEndian(value, 4); }
    void writeU64(u64 value) { writeLittleEndian(value, 8); }
    void writeF64(f64 value) { writeU64(std::bit_cast<u64>(value)); }
    void writeBool(bool value) { writeU8(value ? 1 : 0); }
    void writeString(std::string_view text);
    void writeBytes(std::span<const std::byte> bytes);

    template <typename T>
    void io(const T& value);
    // Named form used by components so debug tools can label fields; the binary layout ignores the name.
    template <typename T>
    void io(std::string_view /*name*/, const T& value) {
        io(value);
    }

    // Writes tag, version and a size placeholder; endChunk patches the size.
    ChunkMark beginChunk(u32 tag, u32 version);
    void endChunk(ChunkMark mark);

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return m_bytes; }
    [[nodiscard]] std::vector<std::byte> takeBytes() { return std::move(m_bytes); }
    [[nodiscard]] usize size() const { return m_bytes.size(); }

private:
    void writeLittleEndian(u64 value, usize byteCount);

    std::vector<std::byte> m_bytes;
};

// Reads what BinaryWriter wrote. Errors are sticky: after the first failure (truncation, bad tag, invalid
// value) every read returns zero/empty and ok() stays false, so callers check once at the end.
class BinaryReader {
public:
    static constexpr bool kIsReading = true;

    struct Chunk {
        u32 tag = 0;
        u32 version = 0;
        usize end = 0;
    };

    explicit BinaryReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    u8 readU8() { return static_cast<u8>(readLittleEndian(1)); }
    u16 readU16() { return static_cast<u16>(readLittleEndian(2)); }
    u32 readU32() { return static_cast<u32>(readLittleEndian(4)); }
    u64 readU64() { return readLittleEndian(8); }
    f64 readF64() { return std::bit_cast<f64>(readU64()); }
    bool readBool();
    std::string readString();
    std::vector<std::byte> readBytes();

    template <typename T>
    void io(T& value);
    template <typename T>
    void io(std::string_view /*name*/, T& value) {
        io(value);
    }

    // Reads a chunk header and checks its tag. Fails (and returns false) on mismatch or truncation.
    bool beginChunk(u32 expectedTag, Chunk& chunk);
    // Skips unread bytes of the chunk (fields appended by newer writers); fails if the chunk was overrun.
    void endChunk(const Chunk& chunk);

    [[nodiscard]] bool ok() const { return m_error.empty(); }
    [[nodiscard]] const std::string& error() const { return m_error; }
    // Records the first error only.
    void fail(std::string message);

    [[nodiscard]] usize position() const { return m_position; }
    [[nodiscard]] usize remaining() const { return m_bytes.size() - m_position; }

private:
    u64 readLittleEndian(usize byteCount);

    std::span<const std::byte> m_bytes;
    usize m_position = 0;
    std::string m_error;
};

// ---------------------------------------------------------------------------------------------------------

template <typename T>
void BinaryWriter::io(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        writeBool(value);
    } else if constexpr (std::is_enum_v<T>) {
        io(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) <= 8);
        writeLittleEndian(static_cast<u64>(value), sizeof(T));
    } else if constexpr (std::is_same_v<T, f64>) {
        writeF64(value);
    } else if constexpr (std::is_same_v<T, f32>) {
        writeU32(std::bit_cast<u32>(value));
    } else if constexpr (std::is_same_v<T, std::string>) {
        writeString(value);
    } else if constexpr (std::is_same_v<T, Vec3d>) {
        writeF64(value.x);
        writeF64(value.y);
        writeF64(value.z);
    } else if constexpr (std::is_same_v<T, SimTime>) {
        io(value.microsecondsSinceEpoch());
    } else if constexpr (std::is_same_v<T, SimDuration>) {
        io(value.count());
    } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
        writeBytes(value);
    } else if constexpr (detail::IsStdVector<T>::value) {
        writeU64(value.size());
        for (const auto& element : value) {
            io(element);
        }
    } else {
        static_assert(HasIoMember<T, BinaryWriter>,
                      "type needs `template <typename Archive> void io(Archive&)`");
        const_cast<T&>(value).io(*this); // writers never modify: io members are shared with the reader
    }
}

template <typename T>
void BinaryReader::io(T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        value = readBool();
    } else if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> raw{};
        io(raw);
        value = static_cast<T>(raw);
    } else if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) <= 8);
        value = static_cast<T>(readLittleEndian(sizeof(T)));
    } else if constexpr (std::is_same_v<T, f64>) {
        value = readF64();
    } else if constexpr (std::is_same_v<T, f32>) {
        value = std::bit_cast<f32>(readU32());
    } else if constexpr (std::is_same_v<T, std::string>) {
        value = readString();
    } else if constexpr (std::is_same_v<T, Vec3d>) {
        value.x = readF64();
        value.y = readF64();
        value.z = readF64();
    } else if constexpr (std::is_same_v<T, SimTime>) {
        i64 us = 0;
        io(us);
        value = SimTime::fromMicroseconds(us);
    } else if constexpr (std::is_same_v<T, SimDuration>) {
        i64 us = 0;
        io(us);
        value = SimDuration::microseconds(us);
    } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
        value = readBytes();
    } else if constexpr (detail::IsStdVector<T>::value) {
        const u64 count = readU64();
        if (count > remaining()) { // every element takes at least one byte: guards corrupt counts
            fail("vector length exceeds the remaining data");
        }
        value.clear();
        if (!ok()) {
            return;
        }
        value.resize(static_cast<usize>(count));
        for (auto& element : value) {
            io(element);
        }
    } else {
        static_assert(HasIoMember<T, BinaryReader>,
                      "type needs `template <typename Archive> void io(Archive&)`");
        value.io(*this);
    }
}

} // namespace gx
