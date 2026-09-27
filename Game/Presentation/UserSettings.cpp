#include "Game/Presentation/UserSettings.h"

#include "Engine/Core/Paths.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace gx {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool parseBool(std::string_view text, bool& out) {
    if (text == "1" || text == "true") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

UserSettings parseUserSettings(std::string_view text) {
    UserSettings settings;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key == "fullscreen") {
            parseBool(value, settings.fullscreen);
        } else if (key == "vsync") {
            parseBool(value, settings.vsync);
        } else if (key == "show_help") {
            parseBool(value, settings.showHelpOnStart);
        } else if (key == "ui_scale") {
            f32 scale = 0.0f;
            const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), scale);
            if (error == std::errc{} && ptr == value.data() + value.size() && !std::isnan(scale)) {
                settings.uiScale = std::clamp(scale, UserSettings::kMinUiScale, UserSettings::kMaxUiScale);
            }
        }
    }
    return settings;
}

std::string formatUserSettings(const UserSettings& settings) {
    return std::format("# GalaxyEngine: preferencias del jugador\n"
                       "fullscreen={}\nvsync={}\nui_scale={:.2f}\nshow_help={}\n",
                       settings.fullscreen ? 1 : 0, settings.vsync ? 1 : 0, settings.uiScale,
                       settings.showHelpOnStart ? 1 : 0);
}

UserSettings loadUserSettings(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseUserSettings(text.str());
}

bool saveUserSettings(const std::filesystem::path& path, const UserSettings& settings, std::string& error) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::string text = formatUserSettings(settings);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error = std::format("cannot write '{}'", pathToUtf8(temporary));
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = std::format("cannot replace '{}': {}", pathToUtf8(path), ec.message());
        return false;
    }
    return true;
}

} // namespace gx
