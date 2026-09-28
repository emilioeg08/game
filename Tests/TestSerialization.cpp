#include "Tests/TestFramework.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Paths.h"
#include "Engine/Serialization/Binary.h"
#include "Engine/Serialization/SaveFile.h"
#include "Game/Presentation/UserSettings.h"

#include <filesystem>
#include <fstream>
#include <limits>

using namespace gx;

namespace {

enum class Faction : u16 { Neutral = 0, Crown = 7, Guild = 300 };

struct Cargo {
    u32 good = 0;
    f64 amount = 0.0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(good);
        ar.io(amount);
    }
    bool operator==(const Cargo&) const = default;
};

struct Everything {
    bool flag = false;
    u8 small = 0;
    i16 negative = 0;
    u32 medium = 0;
    i64 large = 0;
    f32 single = 0.0f;
    f64 precise = 0.0;
    Faction faction = Faction::Neutral;
    std::string name;
    Vec3d position;
    SimTime when;
    SimDuration span;
    std::vector<u32> numbers;
    std::vector<Cargo> hold;
    std::vector<std::byte> blob;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(flag);
        ar.io(small);
        ar.io(negative);
        ar.io(medium);
        ar.io(large);
        ar.io(single);
        ar.io(precise);
        ar.io(faction);
        ar.io(name);
        ar.io(position);
        ar.io(when);
        ar.io(span);
        ar.io(numbers);
        ar.io(hold);
        ar.io(blob);
    }
    bool operator==(const Everything&) const = default;
};

std::vector<std::byte> bytesOf(std::initializer_list<int> values) {
    std::vector<std::byte> bytes;
    for (const int v : values) {
        bytes.push_back(static_cast<std::byte>(v));
    }
    return bytes;
}

std::filesystem::path tempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

GX_TEST(Serialization, LayoutIsLittleEndian) {
    BinaryWriter writer;
    writer.writeU32(0x01020304u);
    writer.io(i16{-2});
    writer.writeF64(1.0);
    const std::vector<std::byte> expected =
        bytesOf({0x04, 0x03, 0x02, 0x01, 0xfe, 0xff, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f});
    GX_EXPECT(writer.bytes() == expected);
}

GX_TEST(Serialization, RoundTripsEverySupportedType) {
    Everything original;
    original.flag = true;
    original.small = 200;
    original.negative = -12345;
    original.medium = 0xdeadbeefu;
    original.large = std::numeric_limits<i64>::min();
    original.single = 1.25f;
    original.precise = -0.0;
    original.faction = Faction::Guild;
    original.name = "Casa Valdria";
    original.position = {1.5e11, -2.0, 3.0e-9};
    original.when = SimTime::epoch() + SimDuration::years(300);
    original.span = SimDuration::minutes(-5);
    original.numbers = {1, 2, 3};
    original.hold = {{4, 10.5}, {9, 0.25}};
    original.blob = bytesOf({1, 2, 3, 255});

    BinaryWriter writer;
    writer.io(original);
    BinaryReader reader(writer.bytes());
    Everything copy;
    reader.io(copy);
    GX_REQUIRE(reader.ok());
    GX_EXPECT_EQ(reader.remaining(), 0u);
    GX_EXPECT(copy == original);
    GX_EXPECT(std::signbit(copy.precise)); // -0.0 survives
}

GX_TEST(Serialization, ChunksSkipFieldsAddedByNewerWriters) {
    BinaryWriter writer;
    const auto mark = writer.beginChunk(fourCC("TEST"), 2);
    writer.io(u32{42});
    writer.io(std::string("field added in version 2"));
    writer.endChunk(mark);
    writer.io(u32{7}); // data after the chunk

    BinaryReader reader(writer.bytes());
    BinaryReader::Chunk chunk;
    GX_REQUIRE(reader.beginChunk(fourCC("TEST"), chunk));
    GX_EXPECT_EQ(chunk.version, 2u);
    u32 known = 0;
    reader.io(known); // a version-1 reader only knows this field
    reader.endChunk(chunk);
    u32 after = 0;
    reader.io(after);
    GX_EXPECT(reader.ok());
    GX_EXPECT_EQ(known, 42u);
    GX_EXPECT_EQ(after, 7u);
}

GX_TEST(Serialization, WrongChunkTagFails) {
    BinaryWriter writer;
    writer.endChunk(writer.beginChunk(fourCC("AAAA"), 1));
    BinaryReader reader(writer.bytes());
    BinaryReader::Chunk chunk;
    GX_EXPECT(!reader.beginChunk(fourCC("BBBB"), chunk));
    GX_EXPECT(!reader.ok());
    GX_EXPECT(reader.error().find("BBBB") != std::string::npos);
}

GX_TEST(Serialization, ErrorsAreSticky) {
    const std::vector<std::byte> data = bytesOf({1, 2});
    BinaryReader reader(data);
    GX_EXPECT_EQ(reader.readU32(), 0u);
    GX_EXPECT(!reader.ok());
    GX_EXPECT(reader.error().find("unexpected end") != std::string::npos);
    GX_EXPECT_EQ(reader.readU8(), 0u); // still failing, still zero
}

GX_TEST(Serialization, CorruptLengthsAreRejected) {
    BinaryWriter hugeString;
    hugeString.writeU32(0xffffffffu);
    BinaryReader stringReader(hugeString.bytes());
    GX_EXPECT(stringReader.readString().empty());
    GX_EXPECT(!stringReader.ok());

    BinaryWriter hugeVector;
    hugeVector.writeU64(1ull << 60);
    BinaryReader vectorReader(hugeVector.bytes());
    std::vector<u64> values;
    vectorReader.io(values); // must fail without trying to allocate 2^60 elements
    GX_EXPECT(!vectorReader.ok());
    GX_EXPECT(values.empty());

    BinaryWriter badBool;
    badBool.writeU8(2);
    BinaryReader boolReader(badBool.bytes());
    (void)boolReader.readBool();
    GX_EXPECT(!boolReader.ok());
}

GX_TEST(Serialization, FourCCNames) {
    static_assert(fourCC("WRLD") == 0x444c5257u);
    GX_EXPECT_EQ(fourCCToString(fourCC("WRLD")), std::string("WRLD"));
    GX_EXPECT_EQ(fourCCToString(0x01000000u), std::string("????"));
}

GX_TEST(Serialization, HashBytesSeesContentAndLength) {
    const std::vector<std::byte> a = bytesOf({1, 2, 3});
    const std::vector<std::byte> b = bytesOf({1, 2, 4});
    const std::vector<std::byte> c = bytesOf({1, 2, 3, 0});
    GX_EXPECT_EQ(hashBytes(a), hashBytes(bytesOf({1, 2, 3})));
    GX_EXPECT(hashBytes(a) != hashBytes(b));
    GX_EXPECT(hashBytes(a) != hashBytes(c)); // trailing zero bytes still change the hash
}

GX_TEST(Serialization, SaveFileRoundTripAndOverwrite) {
    const auto path = tempPath("gx_test_save.gxsave");
    BinaryWriter payload;
    payload.endChunk(payload.beginChunk(fourCC("ONE "), 1));
    const auto mark = payload.beginChunk(fourCC("TWO "), 3);
    payload.writeString("data");
    payload.endChunk(mark);

    std::string error;
    GX_REQUIRE(writeSaveFile(path, "0.1.0", "first", payload.bytes(), error));
    GX_REQUIRE(writeSaveFile(path, "0.1.0", "second", payload.bytes(), error)); // replaces the existing file
    SaveFileContents contents;
    GX_REQUIRE(readSaveFile(path, contents, error));
    std::filesystem::remove(path);

    GX_EXPECT_EQ(contents.info.formatVersion, kSaveFormatVersion);
    GX_EXPECT_EQ(contents.info.description, std::string("second"));
    GX_EXPECT(contents.payload == payload.bytes());
    const auto chunks = listChunks(contents.payload);
    GX_REQUIRE(chunks.size() == 2);
    GX_EXPECT_EQ(fourCCToString(chunks[1].tag), std::string("TWO "));
    GX_EXPECT_EQ(chunks[1].version, 3u);
    GX_EXPECT(!std::filesystem::exists(path.string() + ".tmp"));
}

GX_TEST(Serialization, SaveFileDetectsCorruption) {
    const auto path = tempPath("gx_test_corrupt.gxsave");
    const std::vector<std::byte> payload(1000, std::byte{0x5a});
    std::string error;
    GX_REQUIRE(writeSaveFile(path, "0.1.0", "corrupt me", payload, error));
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-10, std::ios::end);
        file.put('\x00'); // flip one payload byte
    }
    SaveFileContents contents;
    GX_EXPECT(!readSaveFile(path, contents, error));
    GX_EXPECT(error.find("hash mismatch") != std::string::npos);

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "definitely not a save file";
    }
    GX_EXPECT(!readSaveFile(path, contents, error));
    GX_EXPECT(error.find("bad magic") != std::string::npos);
    std::filesystem::remove(path);

    GX_EXPECT(!readSaveFile(tempPath("gx_test_missing.gxsave"), contents, error));
}

GX_TEST(Serialization, SaveFilesLiveInFoldersWithAnyName) {
    // A Windows profile such as C:\\Users\\José: the path must be built from UTF-8, not the ANSI code page.
    const std::string folder = "gx_test_Jos\xc3\xa9_\xc3\xb1";
    GX_EXPECT(pathToUtf8(pathFromUtf8(folder)) == folder);
    GX_EXPECT(pathFromUtf8(folder).u8string() == u8"gx_test_Jos\u00e9_\u00f1");
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / pathFromUtf8(folder);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    GX_REQUIRE(!ec);
    const std::filesystem::path path = directory / pathFromUtf8("partida r\xc3\xa1pida.gxsave");
    const std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    std::string error;
    GX_REQUIRE(writeSaveFile(path, "test", "Partida de Jos\xc3\xa9", payload, error));
    SaveFileContents contents;
    GX_REQUIRE(readSaveFile(path, contents, error));
    GX_EXPECT(contents.payload == payload);
    std::filesystem::remove_all(directory, ec);
}

GX_TEST(Serialization, UserSettingsRoundTripAndTolerateOtherVersions) {
    UserSettings settings;
    settings.fullscreen = true;
    settings.vsync = false;
    settings.uiScale = 1.25f;
    settings.showHelpOnStart = false;
    settings.language = "en";
    GX_EXPECT(parseUserSettings(formatUserSettings(settings)) == settings);
    GX_EXPECT(parseUserSettings("language=../../x").language == "auto"); // not a language code
    GX_EXPECT(parseUserSettings("") == UserSettings{}); // no file: defaults
    // Unknown keys, comments, spaces, Windows line ends, bad values and out-of-range scales.
    const UserSettings odd = parseUserSettings("# comment\r\n future_key = 7\r\nfullscreen = true\r\n"
                                               "vsync=maybe\r\nui_scale=9\r\nshow_help\r\n");
    GX_EXPECT(odd.fullscreen);
    GX_EXPECT(odd.vsync); // unchanged default
    GX_EXPECT_NEAR(odd.uiScale, UserSettings::kMaxUiScale, 1e-6);
    GX_EXPECT(odd.showHelpOnStart);
    GX_EXPECT_NEAR(parseUserSettings("ui_scale=abc").uiScale, 1.0f, 1e-6);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "gx_test_settings.ini";
    std::string error;
    GX_REQUIRE(saveUserSettings(path, settings, error));
    GX_EXPECT(loadUserSettings(path) == settings);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    GX_EXPECT(loadUserSettings(path) == UserSettings{});
}
