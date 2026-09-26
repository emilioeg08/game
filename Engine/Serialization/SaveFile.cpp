#include "Engine/Serialization/SaveFile.h"

#include "Engine/Core/Hash.h"
#include "Engine/Serialization/Binary.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <system_error>

namespace gx {
namespace {

// PNG-style magic: the CR LF / SUB / LF bytes detect text-mode transfers that would corrupt the file.
constexpr std::array<std::byte, 8> kMagic = {std::byte{'G'},  std::byte{'X'},  std::byte{'S'},
                                             std::byte{'V'},  std::byte{0x0d}, std::byte{0x0a},
                                             std::byte{0x1a}, std::byte{0x0a}};

} // namespace

bool writeSaveFile(const std::filesystem::path& path, std::string_view engineVersion,
                   std::string_view description, std::span<const std::byte> payload, std::string& error) {
    BinaryWriter header;
    header.writeU32(kSaveFormatVersion);
    header.writeString(engineVersion);
    header.writeString(description);
    header.writeU64(payload.size());
    header.writeU64(hashBytes(payload));

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = std::format("cannot open '{}' for writing", temporary.string());
            return false;
        }
        out.write(reinterpret_cast<const char*>(kMagic.data()), static_cast<std::streamsize>(kMagic.size()));
        out.write(reinterpret_cast<const char*>(header.bytes().data()),
                  static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out) {
            error = std::format("failed while writing '{}'", temporary.string());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = std::format("cannot replace '{}': {}", path.string(), ec.message());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool readSaveFile(const std::filesystem::path& path, SaveFileContents& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("cannot open '{}'", path.string());
        return false;
    }
    std::vector<std::byte> file;
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0, std::ios::beg);
    if (length < 0) {
        error = std::format("cannot read '{}'", path.string());
        return false;
    }
    file.resize(static_cast<usize>(length));
    in.read(reinterpret_cast<char*>(file.data()), length);
    if (!in) {
        error = std::format("cannot read '{}'", path.string());
        return false;
    }

    if (file.size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), file.begin())) {
        error = "not a GalaxyEngine save file (bad magic)";
        return false;
    }
    BinaryReader reader(std::span<const std::byte>(file).subspan(kMagic.size()));
    SaveFileInfo info;
    info.formatVersion = reader.readU32();
    if (reader.ok() && info.formatVersion != kSaveFormatVersion) {
        error = std::format("unsupported save format version {} (expected {})", info.formatVersion,
                            kSaveFormatVersion);
        return false;
    }
    info.engineVersion = reader.readString();
    info.description = reader.readString();
    info.payloadBytes = reader.readU64();
    info.payloadHash = reader.readU64();
    if (!reader.ok()) {
        error = "corrupt save header: " + reader.error();
        return false;
    }
    if (info.payloadBytes != reader.remaining()) {
        error = std::format("save payload is {} bytes but the header says {}", reader.remaining(),
                            info.payloadBytes);
        return false;
    }
    const auto payload = std::span<const std::byte>(file).subspan(kMagic.size() + reader.position());
    if (hashBytes(payload) != info.payloadHash) {
        error = "save payload is corrupt (hash mismatch)";
        return false;
    }
    out.info = std::move(info);
    out.payload.assign(payload.begin(), payload.end());
    return true;
}

std::vector<ChunkInfo> listChunks(std::span<const std::byte> payload) {
    std::vector<ChunkInfo> chunks;
    BinaryReader reader(payload);
    while (reader.ok() && reader.remaining() > 0) {
        ChunkInfo info;
        info.offset = reader.position();
        info.tag = reader.readU32();
        info.version = reader.readU32();
        info.size = reader.readU64();
        if (!reader.ok() || info.size > reader.remaining()) {
            break;
        }
        chunks.push_back(info);
        BinaryReader::Chunk skip{info.tag, info.version, reader.position() + static_cast<usize>(info.size)};
        reader.endChunk(skip);
    }
    return chunks;
}

} // namespace gx
