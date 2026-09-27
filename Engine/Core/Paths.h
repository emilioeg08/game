#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Paths and UTF-8 (shipping on Windows, ADR-034). The engine keeps text in UTF-8 std::string, but on
// Windows a std::filesystem::path built from a narrow string is decoded with the ANSI code page: a user
// profile such as C:\Users\José\AppData would name another folder. Every path that comes from text (the OS,
// the command line, the UI) goes through these.
namespace gx {

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8);
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);

} // namespace gx
