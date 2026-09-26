#pragma once

#include "Engine/Core/Types.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Save file container: magic, header (format version, engine version, description) and a payload protected
// by its size and hash. The payload format belongs to whoever produced it (the simulation kernel).
namespace gx {

inline constexpr u32 kSaveFormatVersion = 1;

struct SaveFileInfo {
    u32 formatVersion = 0;
    std::string engineVersion;
    std::string description;
    u64 payloadBytes = 0;
    u64 payloadHash = 0;
};

struct SaveFileContents {
    SaveFileInfo info;
    std::vector<std::byte> payload;
};

struct ChunkInfo {
    u32 tag = 0;
    u32 version = 0;
    u64 offset = 0; // of the chunk header, within the payload
    u64 size = 0;   // of the chunk body
};

// Writes to "<path>.tmp" and renames over `path`, so a crash never leaves a half-written save behind.
bool writeSaveFile(const std::filesystem::path& path, std::string_view engineVersion,
                   std::string_view description, std::span<const std::byte> payload, std::string& error);

// Validates magic, format version, size and hash before returning the payload.
bool readSaveFile(const std::filesystem::path& path, SaveFileContents& out, std::string& error);

// Top-level chunks of a payload (for the save inspector). Stops at the first malformed chunk.
[[nodiscard]] std::vector<ChunkInfo> listChunks(std::span<const std::byte> payload);

} // namespace gx
