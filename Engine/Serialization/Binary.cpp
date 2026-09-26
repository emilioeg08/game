#include "Engine/Serialization/Binary.h"

#include <cstring>
#include <format>

namespace gx {

std::string fourCCToString(u32 tag) {
    std::string text(4, '?');
    for (usize i = 0; i < 4; ++i) {
        const auto c = static_cast<char>((tag >> (8 * i)) & 0xff);
        text[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return text;
}

void BinaryWriter::writeLittleEndian(u64 value, usize byteCount) {
    const usize offset = m_bytes.size();
    m_bytes.resize(offset + byteCount);
    for (usize i = 0; i < byteCount; ++i) {
        m_bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
    }
}

void BinaryWriter::writeString(std::string_view text) {
    writeU32(static_cast<u32>(text.size()));
    const usize offset = m_bytes.size();
    m_bytes.resize(offset + text.size());
    if (!text.empty()) {
        std::memcpy(m_bytes.data() + offset, text.data(), text.size());
    }
}

void BinaryWriter::writeBytes(std::span<const std::byte> bytes) {
    writeU64(bytes.size());
    m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
}

BinaryWriter::ChunkMark BinaryWriter::beginChunk(u32 tag, u32 version) {
    writeU32(tag);
    writeU32(version);
    const ChunkMark mark{m_bytes.size()};
    writeU64(0); // patched by endChunk
    return mark;
}

void BinaryWriter::endChunk(ChunkMark mark) {
    const u64 size = m_bytes.size() - (mark.sizeOffset + 8);
    for (usize i = 0; i < 8; ++i) {
        m_bytes[mark.sizeOffset + i] = static_cast<std::byte>((size >> (8 * i)) & 0xff);
    }
}

void BinaryReader::fail(std::string message) {
    if (m_error.empty()) {
        m_error = std::format("{} (at byte {})", message, m_position);
    }
}

u64 BinaryReader::readLittleEndian(usize byteCount) {
    if (!ok()) {
        return 0;
    }
    if (remaining() < byteCount) {
        fail("unexpected end of data");
        m_position = m_bytes.size();
        return 0;
    }
    u64 value = 0;
    for (usize i = 0; i < byteCount; ++i) {
        value |= static_cast<u64>(static_cast<u8>(m_bytes[m_position + i])) << (8 * i);
    }
    m_position += byteCount;
    return value;
}

bool BinaryReader::readBool() {
    const u8 raw = readU8();
    if (raw > 1) {
        fail("invalid boolean");
        return false;
    }
    return raw == 1;
}

std::string BinaryReader::readString() {
    const u32 length = readU32();
    if (length > remaining()) {
        fail("string length exceeds the remaining data");
    }
    if (!ok()) {
        return {};
    }
    std::string text(length, '\0');
    if (length > 0) {
        std::memcpy(text.data(), m_bytes.data() + m_position, length);
    }
    m_position += length;
    return text;
}

std::vector<std::byte> BinaryReader::readBytes() {
    const u64 length = readU64();
    if (length > remaining()) {
        fail("byte block exceeds the remaining data");
    }
    if (!ok()) {
        return {};
    }
    std::vector<std::byte> bytes(m_bytes.begin() + static_cast<std::ptrdiff_t>(m_position),
                                 m_bytes.begin() + static_cast<std::ptrdiff_t>(m_position + length));
    m_position += static_cast<usize>(length);
    return bytes;
}

bool BinaryReader::beginChunk(u32 expectedTag, Chunk& chunk) {
    chunk.tag = readU32();
    chunk.version = readU32();
    const u64 size = readU64();
    if (!ok()) {
        return false;
    }
    if (chunk.tag != expectedTag) {
        fail(std::format("expected chunk '{}' but found '{}'", fourCCToString(expectedTag),
                         fourCCToString(chunk.tag)));
        return false;
    }
    if (size > remaining()) {
        fail(std::format("chunk '{}' is truncated", fourCCToString(chunk.tag)));
        return false;
    }
    chunk.end = m_position + static_cast<usize>(size);
    return true;
}

void BinaryReader::endChunk(const Chunk& chunk) {
    if (!ok()) {
        return;
    }
    if (m_position > chunk.end) {
        fail(std::format("chunk '{}' was read past its end", fourCCToString(chunk.tag)));
        return;
    }
    m_position = chunk.end;
}

} // namespace gx
