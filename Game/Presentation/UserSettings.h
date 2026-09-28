#pragma once

#include "Engine/Core/Types.h"

#include <filesystem>
#include <string>
#include <string_view>

// The player's preferences (ADR-035): a small text file in the user's data folder, `key=value` per line.
// Unknown keys and invalid values are ignored (a file from a newer or older version still loads), and
// values are clamped to what the game supports.
namespace gx {

struct UserSettings {
    static constexpr f32 kMinUiScale = 0.75f;
    static constexpr f32 kMaxUiScale = 2.0f;

    bool fullscreen = false;
    bool vsync = true;
    f32 uiScale = 1.0f; // on top of the display's own scale
    bool showHelpOnStart = true;
    std::string language = "auto"; // "auto" (the system's), "es" or a catalog in data/lang (ADR-036)

    friend bool operator==(const UserSettings&, const UserSettings&) = default;
};

[[nodiscard]] UserSettings parseUserSettings(std::string_view text);
[[nodiscard]] std::string formatUserSettings(const UserSettings& settings);

// Missing or unreadable file: defaults. Writing replaces the file only once the new one is complete.
[[nodiscard]] UserSettings loadUserSettings(const std::filesystem::path& path);
bool saveUserSettings(const std::filesystem::path& path, const UserSettings& settings, std::string& error);

} // namespace gx
